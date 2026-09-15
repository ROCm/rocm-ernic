/*
 * QEMU paravirtual RDMA - QP implementation
 *
 * Copyright (C) 2018 Oracle
 * Copyright (C) 2018 Red Hat Inc
 *
 * Authors:
 *     Yuval Shaia <yuval.shaia@oracle.com>
 *     Marcel Apfelbaum <marcel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

/* Minimal includes instead of qemu/osdep.h */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdatomic.h>

#include "qemu/compiler.h" /* For unlikely() */
#include "../rdma_utils.h"
#include "../rdma_rm.h"
#include "../rdma_backend.h"
#include <glib.h> /* For g_idle_add() */

#include "pvrdma.h"
#include "standard-headers/rdma/vmw_pvrdma-abi.h"
#include "pvrdma_comp_ctx.h"
#include "pvrdma_qp_ops.h"

typedef PvrdmaCompHandlerCtx CompHandlerCtx;

/*
 * Deferred completion -- queued by background threads,
 * drained by the main loop on every iteration.
 */
typedef struct DeferredCompletion {
    PVRDMADev *dev;
    uint32_t cq_handle;
    uint32_t qp_handle;
    struct pvrdma_cqe cqe;
    struct ibv_wc wc;
    bool signaled;
} DeferredCompletion;

static GQueue *g_deferred_completions;
static QemuMutex g_deferred_lock;

/*
 * Record a work completion and poke the main loop to raise the
 * completion MSI-X.
 *
 * The guest-visible CQ ring used to live here: the PVRDMA device wrote
 * the CQE into the ring mapped from the guest CQ and pushed the CQ
 * number onto the DSR completion ring.  Both went away with the
 * PVRDMA device mode -- the ionic front end formats and posts its
 * own ionic_v1_cqe entries from ionic_datapath.c -- so all that is left
 * to do here is the bookkeeping and the interrupt signal.
 */
static int pvrdma_post_cqe(PVRDMADev *dev, uint32_t cq_handle,
                           struct pvrdma_cqe *cqe, struct ibv_wc *wc)
{
    RdmaRmCQ *cq = rdma_rm_get_cq(&dev->rdma_dev_res, cq_handle);
    uint32_t qp_handle = cqe->qp ? cqe->qp : wc->qp_num;
    PVRDMAQPStats *qp_stats;

    if (unlikely(!cq)) {
        rdma_error_report("pvrdma_post_cqe: CQ handle %u not found", cq_handle);
        return -EINVAL;
    }

    rdma_info_report("pvrdma_post_cqe: cq=%u qp=%u opcode=%d status=%d",
                     cq_handle, qp_handle, cqe->opcode, wc->status);

    qp_stats = pvrdma_get_qp_stats(dev, qp_handle);
    if (qp_stats) {
        qp_stats->cqes_posted++;
    }

    if (cq->notify == CNT_ARM) {
        cq->notify = CNT_CLEAR;
    }

    /*
     * Signal the main loop to fire the MSI-X
     * interrupt.  We must not call vfu_irq_trigger
     * from the TCP recv thread because the
     * libvfio-user context is not thread-safe.
     */
    __atomic_store_n(&dev->pending_cq_interrupt, 1, __ATOMIC_RELEASE);

    return 0;
}

static void pvrdma_qp_ops_comp_handler(void *ctx, struct ibv_wc *wc)
{
    CompHandlerCtx *comp_ctx = (CompHandlerCtx *)ctx;

    /* A backend may complete a request that was posted without a context (the
     * ionic datapath posts its own CQEs); there is nothing to defer then. */
    if (!comp_ctx)
        return;

    /*
     * Queue the completion for the main loop to
     * post.  All vfio-user / DMA-mapped memory
     * access must happen on the main thread.
     * send_in_flight is also decremented there to
     * avoid racing rdma_rm_get_qp (g_hash_table)
     * against QP alloc/dealloc on the main thread.
     */
    DeferredCompletion *dc = g_new(DeferredCompletion, 1);
    dc->dev = comp_ctx->dev;
    dc->cq_handle = comp_ctx->cq_handle;
    dc->qp_handle = comp_ctx->qp_handle;
    dc->cqe = comp_ctx->cqe;
    dc->wc = *wc;
    dc->signaled = comp_ctx->signaled;

    qemu_mutex_lock(&g_deferred_lock);
    g_queue_push_tail(g_deferred_completions, dc);
    qemu_mutex_unlock(&g_deferred_lock);

    __atomic_store_n(&comp_ctx->dev->pending_cq_interrupt, 1, __ATOMIC_RELEASE);

    g_free(ctx);
}

static void complete_with_error(uint32_t vendor_err, void *ctx)
{
    struct ibv_wc wc = {};

    wc.status = IBV_WC_GENERAL_ERR;
    wc.vendor_err = vendor_err;

    pvrdma_qp_ops_comp_handler(ctx, &wc);
}

void pvrdma_qp_ops_fini(void)
{
    rdma_backend_unregister_comp_handler();
    if (g_deferred_completions) {
        while (!g_queue_is_empty(g_deferred_completions)) {
            g_free(g_queue_pop_head(g_deferred_completions));
        }
        g_queue_free(g_deferred_completions);
        g_deferred_completions = NULL;
    }
    qemu_mutex_destroy(&g_deferred_lock);
}

int pvrdma_qp_ops_init(void)
{
    g_deferred_completions = g_queue_new();
    qemu_mutex_init(&g_deferred_lock);
    rdma_backend_register_comp_handler(pvrdma_qp_ops_comp_handler);

    return 0;
}

void pvrdma_queue_recv_work_completion(PVRDMADev *dev, uint32_t recv_cq_handle,
                                       uint64_t recv_guest_wr_id,
                                       uint32_t byte_len, uint32_t src_qp_num)
{
    DeferredCompletion *d;

    if (!g_deferred_completions) {
        return;
    }

    d = g_new(DeferredCompletion, 1);
    d->dev = dev;
    d->cq_handle = recv_cq_handle;
    d->qp_handle = 0;
    memset(&d->cqe, 0, sizeof(d->cqe));
    d->cqe.wr_id = recv_guest_wr_id;
    d->cqe.qp = src_qp_num;
    d->cqe.opcode = IBV_WC_RECV;

    memset(&d->wc, 0, sizeof(d->wc));
    d->wc.status = IBV_WC_SUCCESS;
    d->wc.byte_len = byte_len;
    d->wc.qp_num = src_qp_num;
    d->wc.opcode = IBV_WC_RECV;
    d->wc.wr_id = recv_guest_wr_id;
    d->wc.src_qp = src_qp_num;

    qemu_mutex_lock(&g_deferred_lock);
    g_queue_push_tail(g_deferred_completions, d);
    qemu_mutex_unlock(&g_deferred_lock);

    __atomic_store_n(&dev->pending_cq_interrupt, 1, __ATOMIC_RELEASE);
}

void pvrdma_queue_recv_imm_work_completion(
    PVRDMADev *dev, uint32_t recv_cq_handle, uint32_t recv_qp_handle,
    uint64_t recv_guest_wr_id, uint32_t byte_len, uint32_t src_qp_num,
    uint32_t imm_data)
{
    DeferredCompletion *d;

    if (!g_deferred_completions) {
        return;
    }

    d = g_new(DeferredCompletion, 1);
    d->dev = dev;
    d->cq_handle = recv_cq_handle;
    d->qp_handle = 0;
    memset(&d->cqe, 0, sizeof(d->cqe));
    d->cqe.wr_id = recv_guest_wr_id;
    d->cqe.qp = recv_qp_handle;
    d->cqe.opcode = IBV_WC_RECV_RDMA_WITH_IMM;

    memset(&d->wc, 0, sizeof(d->wc));
    d->wc.status = IBV_WC_SUCCESS;
    d->wc.byte_len = byte_len;
    d->wc.qp_num = recv_qp_handle;
    d->wc.opcode = IBV_WC_RECV_RDMA_WITH_IMM;
    d->wc.wc_flags = IBV_WC_WITH_IMM;
    d->wc.imm_data = imm_data;
    d->wc.wr_id = recv_guest_wr_id;
    d->wc.src_qp = src_qp_num;

    qemu_mutex_lock(&g_deferred_lock);
    g_queue_push_tail(g_deferred_completions, d);
    qemu_mutex_unlock(&g_deferred_lock);

    __atomic_store_n(&dev->pending_cq_interrupt, 1, __ATOMIC_RELEASE);
}

void pvrdma_drain_deferred_completions(void)
{
    if (!g_deferred_completions) {
        return;
    }

    for (;;) {
        qemu_mutex_lock(&g_deferred_lock);
        DeferredCompletion *dc = g_queue_pop_head(g_deferred_completions);
        qemu_mutex_unlock(&g_deferred_lock);

        if (!dc) {
            break;
        }

        if (dc->qp_handle) {
            RdmaRmQP *qp =
                rdma_rm_get_qp(&dc->dev->rdma_dev_res, dc->qp_handle);
            if (qp) {
                atomic_fetch_sub_explicit(&qp->send_in_flight, 1,
                                          memory_order_acq_rel);
            }
        }

        rdma_info_report("DRAIN: posting CQE cq=%u qp=%u opcode=%d "
                         "status=%d byte_len=%u wr_id=%lu",
                         dc->cq_handle, dc->cqe.qp ? dc->cqe.qp : dc->wc.qp_num,
                         dc->cqe.opcode, dc->wc.status, dc->wc.byte_len,
                         (unsigned long)dc->cqe.wr_id);

        /*
         * An unsignalled send that succeeded must not consume a
         * CQ slot.  Posting one anyway overruns the CQ whenever
         * the application moderates completions -- perftest
         * signals every cq_mod-th WR by default -- and the
         * dropped CQEs are the ones it is waiting for.
         * Errors are always reported, signalled or not.
         */
        if (dc->qp_handle && !dc->signaled && dc->wc.status == IBV_WC_SUCCESS) {
            g_free(dc);
            continue;
        }

        int post_rc =
            pvrdma_post_cqe(dc->dev, dc->cq_handle, &dc->cqe, &dc->wc);
        if (post_rc) {
            rdma_error_report("DRAIN: pvrdma_post_cqe FAILED rc=%d "
                              "cq=%u qp=%u",
                              post_rc, dc->cq_handle,
                              dc->cqe.qp ? dc->cqe.qp : dc->wc.qp_num);
        } else {
            rdma_info_report("DRAIN: CQE posted successfully to cq=%u",
                             dc->cq_handle);
        }
        g_free(dc);
    }
}

