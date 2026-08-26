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

/* Number of WQEs to process per batch before yielding to event loop */
#define WQE_BATCH_SIZE 16

/*
 * Max send WQEs posted to backend but not yet completed, per QP.
 * Kept small to prevent TCP buffer exhaustion: the TCP send path
 * busy-waits when the socket buffer is full, blocking the main
 * thread and preventing completion delivery.  At 4 WQEs with
 * 1 MB messages the total in-flight is 4 MB, within the default
 * TCP socket buffer (net.core.wmem_max).
 */
#define MAX_SEND_IN_FLIGHT 4

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
} DeferredCompletion;

static GQueue *g_deferred_completions;
static QemuMutex g_deferred_lock;

/* Send Queue WQE */
typedef struct PvrdmaSqWqe {
    struct pvrdma_sq_wqe_hdr hdr;
    struct pvrdma_sge sge[];
} PvrdmaSqWqe;

/* Recv Queue WQE */
typedef struct PvrdmaRqWqe {
    struct pvrdma_rq_wqe_hdr hdr;
    struct pvrdma_sge sge[];
} PvrdmaRqWqe;

/*
 * 1. Put CQE on send CQ ring
 * 2. Put CQ number on dsr completion ring
 * 3. Interrupt host
 */
static int pvrdma_post_cqe(PVRDMADev *dev, uint32_t cq_handle,
                           struct pvrdma_cqe *cqe, struct ibv_wc *wc)
{
    struct pvrdma_cqe *cqe1;
    struct pvrdma_cqne *cqne;
    PvrdmaRing *ring;
    RdmaRmCQ *cq = rdma_rm_get_cq(&dev->rdma_dev_res, cq_handle);
    uint32_t qp_handle = cqe->qp ? cqe->qp : wc->qp_num;
    PVRDMAQPStats *qp_stats;

    if (unlikely(!cq)) {
        rdma_error_report("pvrdma_post_cqe: CQ handle %u not found", cq_handle);
        return -EINVAL;
    }

    ring = (PvrdmaRing *)cq->opaque;

    uint32_t pre_prod =
        __atomic_load_n(&ring->ring_state->prod_tail, __ATOMIC_ACQUIRE);
    uint32_t pre_cons =
        __atomic_load_n(&ring->ring_state->cons_head, __ATOMIC_ACQUIRE);

    /* Step #1: Put CQE on CQ ring */
    cqe1 = pvrdma_ring_next_elem_write(ring);
    if (unlikely(!cqe1)) {
        rdma_error_report("pvrdma_post_cqe: CQ ring full cq=%u "
                          "prod=%u cons=%u",
                          cq_handle, pre_prod, pre_cons);
        return -EINVAL;
    }

    memset(cqe1, 0, sizeof(*cqe1));
    cqe1->wr_id = cqe->wr_id;
    cqe1->qp = qp_handle;
    cqe1->opcode = cqe->opcode;
    cqe1->status = wc->status;
    cqe1->byte_len = wc->byte_len;
    cqe1->src_qp = wc->src_qp;
    cqe1->wc_flags = wc->wc_flags;
    cqe1->vendor_err = wc->vendor_err;

    pvrdma_ring_write_inc(ring);

    uint32_t post_prod =
        __atomic_load_n(&ring->ring_state->prod_tail, __ATOMIC_ACQUIRE);
    rdma_info_report("pvrdma_post_cqe: cq=%u qp=%u opcode=%d status=%d "
                     "ring prod %u->%u cons=%u",
                     cq_handle, qp_handle, cqe->opcode, wc->status, pre_prod,
                     post_prod, pre_cons);

    /* Track CQE posting */
    qp_stats = pvrdma_get_qp_stats(dev, qp_handle);
    if (qp_stats) {
        qp_stats->cqes_posted++;
    }

    /* Step #2: Put CQ number on dsr completion ring */
    cqne = pvrdma_ring_next_elem_write(&dev->dsr_info.cq);
    if (unlikely(!cqne)) {
        return -EINVAL;
    }

    cqne->info = cq_handle;
    pvrdma_ring_write_inc(&dev->dsr_info.cq);

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
                __atomic_fetch_sub(&qp->send_in_flight, 1, __ATOMIC_ACQ_REL);
            }
        }

        rdma_info_report("DRAIN: posting CQE cq=%u qp=%u opcode=%d "
                         "status=%d byte_len=%u wr_id=%lu",
                         dc->cq_handle, dc->cqe.qp ? dc->cqe.qp : dc->wc.qp_num,
                         dc->cqe.opcode, dc->wc.status, dc->wc.byte_len,
                         (unsigned long)dc->cqe.wr_id);

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

/* Map PVRDMA opcode to IBV completion opcode */
static enum ibv_wc_opcode pvrdma_to_ibv_wc_opcode(uint32_t pvrdma_opcode)
{
    switch (pvrdma_opcode) {
    case PVRDMA_WR_SEND:
    case PVRDMA_WR_SEND_WITH_IMM:
    case PVRDMA_WR_SEND_WITH_INV:
    case PVRDMA_WR_SEND_DC:
        return IBV_WC_SEND;
    case PVRDMA_WR_RDMA_WRITE:
    case PVRDMA_WR_RDMA_WRITE_WITH_IMM:
        return IBV_WC_RDMA_WRITE;
    case PVRDMA_WR_RDMA_READ:
    case PVRDMA_WR_RDMA_READ_WITH_INV:
        return IBV_WC_RDMA_READ;
    case PVRDMA_WR_ATOMIC_CMP_AND_SWP:
    case PVRDMA_WR_MASKED_ATOMIC_CMP_AND_SWP:
        return IBV_WC_COMP_SWAP;
    case PVRDMA_WR_ATOMIC_FETCH_AND_ADD:
    case PVRDMA_WR_MASKED_ATOMIC_FETCH_AND_ADD:
        return IBV_WC_FETCH_ADD;
    default:
        return IBV_WC_SEND;
    }
}

/* Context for QP continuation callback */
typedef struct QPContinuationCtx {
    PVRDMADev *dev;
    uint32_t qp_handle;
} QPContinuationCtx;

/* Context for SRQ continuation callback */
typedef struct SRQContinuationCtx {
    PVRDMADev *dev;
    uint32_t srq_handle;
} SRQContinuationCtx;

/* Continuation callback for QP recv processing */
static gboolean continue_qp_recv_processing(gpointer user_data)
{
    QPContinuationCtx *ctx = (QPContinuationCtx *)user_data;
    PVRDMADev *dev = ctx->dev;
    uint32_t qp_handle = ctx->qp_handle;
    RdmaRmQP *qp;
    PvrdmaRqWqe *wqe;
    PvrdmaRing *ring;
    bool more_wqes = false;

    qp = rdma_rm_get_qp(&dev->rdma_dev_res, qp_handle);
    if (unlikely(!qp)) {
        g_free(ctx);
        return G_SOURCE_REMOVE;
    }

    ring = &((PvrdmaRing *)qp->opaque)[1];

    qp->wqe_state.recv_wqes_processed = 0;

    wqe = pvrdma_ring_next_elem_read(ring);

    while (wqe && qp->wqe_state.recv_wqes_processed < WQE_BATCH_SIZE) {
        CompHandlerCtx *comp_ctx;

        comp_ctx = g_new0(CompHandlerCtx, 1);
        comp_ctx->dev = dev;
        comp_ctx->cq_handle = qp->recv_cq_handle;
        comp_ctx->qp_handle = 0;
        comp_ctx->cqe.wr_id = wqe->hdr.wr_id;
        comp_ctx->cqe.qp = qp_handle;
        comp_ctx->cqe.opcode = IBV_WC_RECV;

        if (wqe->hdr.num_sge > dev->dev_attr.max_sge) {
            rdma_error_report("Invalid num_sge=%d (max %d)", wqe->hdr.num_sge,
                              dev->dev_attr.max_sge);
            complete_with_error(VENDOR_ERR_INV_NUM_SGE, comp_ctx);
            pvrdma_ring_read_inc(ring);
            qp->wqe_state.recv_wqes_processed++;
            wqe = pvrdma_ring_next_elem_read(ring);
            continue;
        }

        {
            PVRDMAQPStats *qp_stats = pvrdma_get_qp_stats(dev, qp_handle);
            if (qp_stats) {
                qp_stats->wqes_processed++;
                qp_stats->wqes_by_opcode[PVRDMA_WR_SEND]++;
            }
        }

        rdma_backend_post_recv(&dev->backend_dev, &qp->backend_qp, qp->qp_type,
                               (struct ibv_sge *)&wqe->sge[0], wqe->hdr.num_sge,
                               comp_ctx);

        pvrdma_ring_read_inc(ring);
        qp->wqe_state.recv_wqes_processed++;
        wqe = pvrdma_ring_next_elem_read(ring);
    }

    if (wqe) {
        more_wqes = true;
    } else {
        qp->wqe_state.recv_processing_active = false;
        qp->wqe_state.recv_wqes_processed = 0;
    }

    g_free(ctx);

    {
        PVRDMAQPStats *qp_stats = pvrdma_get_qp_stats(dev, qp_handle);
        if (qp_stats && more_wqes) {
            qp_stats->continuations++;
        }
    }

    if (more_wqes) {
        QPContinuationCtx *new_ctx = g_new(QPContinuationCtx, 1);
        new_ctx->dev = dev;
        new_ctx->qp_handle = qp_handle;
        g_idle_add(continue_qp_recv_processing, new_ctx);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_REMOVE;
}

static void schedule_recv_processing_continuation(PVRDMADev *dev,
                                                  uint32_t qp_handle)
{
    QPContinuationCtx *ctx = g_new(QPContinuationCtx, 1);
    ctx->dev = dev;
    ctx->qp_handle = qp_handle;
    g_idle_add(continue_qp_recv_processing, ctx);
}

/* Continuation callback for QP send processing */
static gboolean continue_qp_send_processing(gpointer user_data)
{
    QPContinuationCtx *ctx = (QPContinuationCtx *)user_data;
    PVRDMADev *dev = ctx->dev;
    uint32_t qp_handle = ctx->qp_handle;
    RdmaRmQP *qp;
    PvrdmaSqWqe *wqe;
    PvrdmaRing *ring;
    int sgid_idx = 0;
    union ibv_gid *sgid = NULL;
    union ibv_gid *dgid = NULL;
    uint32_t dqpn = 0;
    uint32_t dqkey = 0;
    bool more_wqes = false;

    qp = rdma_rm_get_qp(&dev->rdma_dev_res, qp_handle);
    if (unlikely(!qp)) {
        g_free(ctx);
        return G_SOURCE_REMOVE;
    }

    ring = (PvrdmaRing *)qp->opaque;

    qp->wqe_state.send_wqes_processed = 0;

    wqe = pvrdma_ring_next_elem_read(ring);

    while (wqe && qp->wqe_state.send_wqes_processed < WQE_BATCH_SIZE) {
        CompHandlerCtx *comp_ctx;
        uint32_t pvrdma_opcode = wqe->hdr.opcode;

        if (__atomic_load_n(&qp->send_in_flight, __ATOMIC_ACQUIRE) >=
            MAX_SEND_IN_FLIGHT) {
            break;
        }

        comp_ctx = g_new0(CompHandlerCtx, 1);
        comp_ctx->dev = dev;
        comp_ctx->cq_handle = qp->send_cq_handle;
        comp_ctx->qp_handle = qp_handle;
        comp_ctx->cqe.wr_id = wqe->hdr.wr_id;
        comp_ctx->cqe.qp = qp_handle;
        comp_ctx->opcode = pvrdma_opcode;
        comp_ctx->remote_addr = 0;
        comp_ctx->rkey = 0;

        comp_ctx->cqe.opcode = pvrdma_to_ibv_wc_opcode(pvrdma_opcode);

        if (qp->qp_type == IBV_QPT_UD) {
            sgid = rdma_rm_get_gid(&dev->rdma_dev_res,
                                   wqe->hdr.wr.ud.av.gid_index);
            if (!sgid) {
                rdma_error_report("Failed to get gid for idx %d",
                                  wqe->hdr.wr.ud.av.gid_index);
                complete_with_error(VENDOR_ERR_INV_GID_IDX, comp_ctx);
                pvrdma_ring_read_inc(ring);
                wqe = pvrdma_ring_next_elem_read(ring);
                qp->wqe_state.send_wqes_processed++;
                continue;
            }

            sgid_idx = rdma_rm_get_backend_gid_index(
                &dev->rdma_dev_res, &dev->backend_dev,
                wqe->hdr.wr.ud.av.gid_index);
            if ((int8_t)sgid_idx < 0) {
                rdma_error_report("Failed to get bk sgid_idx for sgid_idx %d",
                                  wqe->hdr.wr.ud.av.gid_index);
                complete_with_error(VENDOR_ERR_INV_GID_IDX, comp_ctx);
                pvrdma_ring_read_inc(ring);
                wqe = pvrdma_ring_next_elem_read(ring);
                qp->wqe_state.send_wqes_processed++;
                continue;
            }

            dgid = (union ibv_gid *)wqe->hdr.wr.ud.av.dgid;
            dqpn = wqe->hdr.wr.ud.remote_qpn;
            dqkey = wqe->hdr.wr.ud.remote_qkey;
        } else if (qp->qp_type == IBV_QPT_RC || qp->qp_type == IBV_QPT_UC) {
            if (pvrdma_opcode == PVRDMA_WR_RDMA_READ ||
                pvrdma_opcode == PVRDMA_WR_RDMA_WRITE ||
                pvrdma_opcode == PVRDMA_WR_RDMA_WRITE_WITH_IMM ||
                pvrdma_opcode == PVRDMA_WR_RDMA_READ_WITH_INV) {
                comp_ctx->remote_addr = wqe->hdr.wr.rdma.remote_addr;
                comp_ctx->rkey = wqe->hdr.wr.rdma.rkey;
            }

            sgid_idx = 0;
            sgid = rdma_rm_get_gid(&dev->rdma_dev_res, 0);
            dgid = NULL;
            dqpn = 0;
            dqkey = 0;
        } else if (qp->qp_type == ROCM_ERNIC_PVRDMA_QPT_DCI ||
                   qp->dc_role == ROCM_ERNIC_DC_ROLE_DCI) {
            if (pvrdma_opcode == PVRDMA_WR_SEND_DC) {
                RdmaRmQP *dct = rdma_rm_lookup_dct(&dev->rdma_dev_res,
                                                   wqe->hdr.wr.dc.remote_dctn);
                uint64_t wire_key = (uint64_t)wqe->hdr.wr.dc.dc_access_key;

                if (!dct) {
                    complete_with_error(VENDOR_ERR_INV_DCT, comp_ctx);
                    pvrdma_ring_read_inc(ring);
                    wqe = pvrdma_ring_next_elem_read(ring);
                    qp->wqe_state.send_wqes_processed++;
                    continue;
                }
                if (dct->dct_access_key != wire_key) {
                    complete_with_error(VENDOR_ERR_DC_KEY, comp_ctx);
                    pvrdma_ring_read_inc(ring);
                    wqe = pvrdma_ring_next_elem_read(ring);
                    qp->wqe_state.send_wqes_processed++;
                    continue;
                }
                if (!dct->is_srq) {
                    complete_with_error(VENDOR_ERR_INV_DCT, comp_ctx);
                    pvrdma_ring_read_inc(ring);
                    wqe = pvrdma_ring_next_elem_read(ring);
                    qp->wqe_state.send_wqes_processed++;
                    continue;
                }
                RdmaRmSRQ *srq_rm =
                    rdma_rm_get_srq(&dev->rdma_dev_res, dct->bound_srq_handle);
                if (!srq_rm) {
                    complete_with_error(VENDOR_ERR_INV_DCT, comp_ctx);
                    pvrdma_ring_read_inc(ring);
                    wqe = pvrdma_ring_next_elem_read(ring);
                    qp->wqe_state.send_wqes_processed++;
                    continue;
                }
                comp_ctx->dc_target_srq = &srq_rm->backend_srq;
                comp_ctx->dc_recv_cq_handle = srq_rm->recv_cq_handle;
                comp_ctx->dc_src_qp = qp->qpn;
            } else if (pvrdma_opcode == PVRDMA_WR_RDMA_READ ||
                       pvrdma_opcode == PVRDMA_WR_RDMA_WRITE ||
                       pvrdma_opcode == PVRDMA_WR_RDMA_WRITE_WITH_IMM ||
                       pvrdma_opcode == PVRDMA_WR_RDMA_READ_WITH_INV) {
                comp_ctx->remote_addr = wqe->hdr.wr.rdma.remote_addr;
                comp_ctx->rkey = wqe->hdr.wr.rdma.rkey;
            } else {
                rdma_error_report("Unsupported opcode %u on DCI QP",
                                  pvrdma_opcode);
                complete_with_error(VENDOR_ERR_INV_QP_TYPE, comp_ctx);
                pvrdma_ring_read_inc(ring);
                wqe = pvrdma_ring_next_elem_read(ring);
                qp->wqe_state.send_wqes_processed++;
                continue;
            }
            sgid_idx = 0;
            sgid = rdma_rm_get_gid(&dev->rdma_dev_res, 0);
            dgid = NULL;
            dqpn = 0;
            dqkey = 0;
        } else {
            rdma_error_report("Unsupported QP type %d for send", qp->qp_type);
            complete_with_error(VENDOR_ERR_INV_QP_TYPE, comp_ctx);
            pvrdma_ring_read_inc(ring);
            wqe = pvrdma_ring_next_elem_read(ring);
            qp->wqe_state.send_wqes_processed++;
            continue;
        }

        if (wqe->hdr.num_sge > dev->dev_attr.max_sge) {
            rdma_error_report("Invalid num_sge=%d (max %d)", wqe->hdr.num_sge,
                              dev->dev_attr.max_sge);
            complete_with_error(VENDOR_ERR_INV_NUM_SGE, comp_ctx);
            pvrdma_ring_read_inc(ring);
            wqe = pvrdma_ring_next_elem_read(ring);
            qp->wqe_state.send_wqes_processed++;
            continue;
        }

        __atomic_fetch_add(&qp->send_in_flight, 1, __ATOMIC_ACQ_REL);

        rdma_backend_post_send(&dev->backend_dev, &qp->backend_qp, qp->qp_type,
                               (struct ibv_sge *)&wqe->sge[0], wqe->hdr.num_sge,
                               sgid_idx, sgid, dgid, dqpn, dqkey, comp_ctx);

        pvrdma_ring_read_inc(ring);
        qp->wqe_state.send_wqes_processed++;
        wqe = pvrdma_ring_next_elem_read(ring);
    }

    if (wqe) {
        more_wqes = true;
    } else {
        qp->wqe_state.send_processing_active = false;
        qp->wqe_state.send_wqes_processed = 0;
    }

    g_free(ctx);

    {
        PVRDMAQPStats *qp_stats = pvrdma_get_qp_stats(dev, qp_handle);
        if (qp_stats && more_wqes) {
            qp_stats->continuations++;
        }
    }

    if (more_wqes) {
        QPContinuationCtx *new_ctx = g_new(QPContinuationCtx, 1);
        new_ctx->dev = dev;
        new_ctx->qp_handle = qp_handle;
        g_idle_add(continue_qp_send_processing, new_ctx);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_REMOVE;
}

static void schedule_wqe_processing_continuation(PVRDMADev *dev,
                                                 uint32_t qp_handle)
{
    QPContinuationCtx *ctx = g_new(QPContinuationCtx, 1);
    ctx->dev = dev;
    ctx->qp_handle = qp_handle;
    g_idle_add(continue_qp_send_processing, ctx);
}

/* Continuation callback for SRQ recv processing */
static gboolean continue_srq_recv_processing(gpointer user_data)
{
    SRQContinuationCtx *ctx = (SRQContinuationCtx *)user_data;
    PVRDMADev *dev = ctx->dev;
    uint32_t srq_handle = ctx->srq_handle;
    RdmaRmSRQ *srq;
    PvrdmaRqWqe *wqe;
    PvrdmaRing *ring;
    bool more_wqes = false;

    srq = rdma_rm_get_srq(&dev->rdma_dev_res, srq_handle);
    if (unlikely(!srq)) {
        g_free(ctx);
        return G_SOURCE_REMOVE;
    }

    ring = (PvrdmaRing *)srq->opaque;

    /* Reset counter for this batch */
    srq->wqe_state.wqes_processed = 0;

    wqe = pvrdma_ring_next_elem_read(ring);

    /* Process batch of WQEs */
    while (wqe && srq->wqe_state.wqes_processed < WQE_BATCH_SIZE) {
        CompHandlerCtx *comp_ctx;

        /* Prepare CQE */
        comp_ctx = g_new0(CompHandlerCtx, 1);
        comp_ctx->dev = dev;
        comp_ctx->cq_handle = srq->recv_cq_handle;
        comp_ctx->qp_handle = 0;
        comp_ctx->cqe.wr_id = wqe->hdr.wr_id;
        comp_ctx->cqe.qp = 0;
        comp_ctx->cqe.opcode = IBV_WC_RECV;

        if (wqe->hdr.num_sge > dev->dev_attr.max_sge) {
            rdma_error_report("Invalid num_sge=%d (max %d)", wqe->hdr.num_sge,
                              dev->dev_attr.max_sge);
            complete_with_error(VENDOR_ERR_INV_NUM_SGE, comp_ctx);
            pvrdma_ring_read_inc(ring);
            srq->wqe_state.wqes_processed++;
            wqe = pvrdma_ring_next_elem_read(ring);
            continue;
        }

        rdma_backend_post_srq_recv(&dev->backend_dev, &srq->backend_srq,
                                   (struct ibv_sge *)&wqe->sge[0],
                                   wqe->hdr.num_sge, comp_ctx);

        pvrdma_ring_read_inc(ring);
        srq->wqe_state.wqes_processed++;
        wqe = pvrdma_ring_next_elem_read(ring);
    }

    /* Check if more WQEs remain */
    if (wqe) {
        more_wqes = true;
    } else {
        srq->wqe_state.processing_active = false;
        srq->wqe_state.wqes_processed = 0;
    }

    g_free(ctx);

    /* Reschedule if more WQEs remain */
    if (more_wqes) {
        SRQContinuationCtx *new_ctx = g_new(SRQContinuationCtx, 1);
        new_ctx->dev = dev;
        new_ctx->srq_handle = srq_handle;
        g_idle_add(continue_srq_recv_processing, new_ctx);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_REMOVE;
}

static void schedule_srq_recv_processing_continuation(PVRDMADev *dev,
                                                      uint32_t srq_handle)
{
    SRQContinuationCtx *ctx = g_new(SRQContinuationCtx, 1);
    ctx->dev = dev;
    ctx->srq_handle = srq_handle;
    g_idle_add(continue_srq_recv_processing, ctx);
}

void pvrdma_qp_send(PVRDMADev *dev, uint32_t qp_handle)
{
    RdmaRmQP *qp;
    PvrdmaSqWqe *wqe;
    PvrdmaRing *ring;
    int sgid_idx = 0;
    union ibv_gid *sgid = NULL;
    union ibv_gid *dgid = NULL;
    uint32_t dqpn = 0;
    uint32_t dqkey = 0;

    rdma_info_report(">>> DOORBELL: pvrdma_qp_send called for qp_handle=%u",
                     qp_handle);

    qp = rdma_rm_get_qp(&dev->rdma_dev_res, qp_handle);
    if (unlikely(!qp)) {
        rdma_error_report(">>> DOORBELL: QP not found for handle %u",
                          qp_handle);
        return;
    }

    rdma_info_report(">>> DOORBELL: Found QP, qp_type=%d", qp->qp_type);

    if (qp->wqe_state.send_processing_active) {
        rdma_info_report(">>> DOORBELL: QP %u send already processing, "
                         "continuation scheduled",
                         qp_handle);
        return;
    }

    ring = (PvrdmaRing *)qp->opaque;

    rdma_info_report(">>> DOORBELL: Getting WQE from ring=%p", ring);
    wqe = pvrdma_ring_next_elem_read(ring);
    rdma_info_report(">>> DOORBELL: Got WQE=%p", wqe);

    if (!wqe) {
        return;
    }

    qp->wqe_state.send_processing_active = true;
    qp->wqe_state.send_wqes_processed = 0;

    while (wqe && qp->wqe_state.send_wqes_processed < WQE_BATCH_SIZE) {
        CompHandlerCtx *comp_ctx;
        uint32_t pvrdma_opcode = wqe->hdr.opcode;

        if (__atomic_load_n(&qp->send_in_flight, __ATOMIC_ACQUIRE) >=
            MAX_SEND_IN_FLIGHT) {
            break;
        }

        comp_ctx = g_new0(CompHandlerCtx, 1);
        comp_ctx->dev = dev;
        comp_ctx->cq_handle = qp->send_cq_handle;
        comp_ctx->qp_handle = qp_handle;
        comp_ctx->cqe.wr_id = wqe->hdr.wr_id;
        comp_ctx->cqe.qp = qp_handle;
        comp_ctx->opcode = pvrdma_opcode;
        comp_ctx->remote_addr = 0;
        comp_ctx->rkey = 0;

        comp_ctx->cqe.opcode = pvrdma_to_ibv_wc_opcode(pvrdma_opcode);

        if (qp->qp_type == IBV_QPT_UD) {
            sgid = rdma_rm_get_gid(&dev->rdma_dev_res,
                                   wqe->hdr.wr.ud.av.gid_index);
            if (!sgid) {
                rdma_error_report("Failed to get gid for idx %d",
                                  wqe->hdr.wr.ud.av.gid_index);
                complete_with_error(VENDOR_ERR_INV_GID_IDX, comp_ctx);
                pvrdma_ring_read_inc(ring);
                qp->wqe_state.send_wqes_processed++;
                wqe = pvrdma_ring_next_elem_read(ring);
                continue;
            }

            sgid_idx = rdma_rm_get_backend_gid_index(
                &dev->rdma_dev_res, &dev->backend_dev,
                wqe->hdr.wr.ud.av.gid_index);
            if ((int8_t)sgid_idx < 0) {
                rdma_error_report("Failed to get bk sgid_idx for sgid_idx %d",
                                  wqe->hdr.wr.ud.av.gid_index);
                complete_with_error(VENDOR_ERR_INV_GID_IDX, comp_ctx);
                pvrdma_ring_read_inc(ring);
                qp->wqe_state.send_wqes_processed++;
                wqe = pvrdma_ring_next_elem_read(ring);
                continue;
            }

            dgid = (union ibv_gid *)wqe->hdr.wr.ud.av.dgid;
            dqpn = wqe->hdr.wr.ud.remote_qpn;
            dqkey = wqe->hdr.wr.ud.remote_qkey;
        } else if (qp->qp_type == IBV_QPT_RC || qp->qp_type == IBV_QPT_UC) {
            if (pvrdma_opcode == PVRDMA_WR_RDMA_READ ||
                pvrdma_opcode == PVRDMA_WR_RDMA_WRITE ||
                pvrdma_opcode == PVRDMA_WR_RDMA_WRITE_WITH_IMM ||
                pvrdma_opcode == PVRDMA_WR_RDMA_READ_WITH_INV) {
                comp_ctx->remote_addr = wqe->hdr.wr.rdma.remote_addr;
                comp_ctx->rkey = wqe->hdr.wr.rdma.rkey;
            }

            sgid_idx = 0;
            sgid = rdma_rm_get_gid(&dev->rdma_dev_res, 0);
            dgid = NULL;
            dqpn = 0;
            dqkey = 0;
        } else if (qp->qp_type == ROCM_ERNIC_PVRDMA_QPT_DCI ||
                   qp->dc_role == ROCM_ERNIC_DC_ROLE_DCI) {
            if (pvrdma_opcode == PVRDMA_WR_SEND_DC) {
                RdmaRmQP *dct = rdma_rm_lookup_dct(&dev->rdma_dev_res,
                                                   wqe->hdr.wr.dc.remote_dctn);
                uint64_t wire_key = (uint64_t)wqe->hdr.wr.dc.dc_access_key;

                if (!dct) {
                    complete_with_error(VENDOR_ERR_INV_DCT, comp_ctx);
                    pvrdma_ring_read_inc(ring);
                    wqe = pvrdma_ring_next_elem_read(ring);
                    qp->wqe_state.send_wqes_processed++;
                    continue;
                }
                if (dct->dct_access_key != wire_key) {
                    complete_with_error(VENDOR_ERR_DC_KEY, comp_ctx);
                    pvrdma_ring_read_inc(ring);
                    wqe = pvrdma_ring_next_elem_read(ring);
                    qp->wqe_state.send_wqes_processed++;
                    continue;
                }
                if (!dct->is_srq) {
                    complete_with_error(VENDOR_ERR_INV_DCT, comp_ctx);
                    pvrdma_ring_read_inc(ring);
                    wqe = pvrdma_ring_next_elem_read(ring);
                    qp->wqe_state.send_wqes_processed++;
                    continue;
                }
                RdmaRmSRQ *srq_rm =
                    rdma_rm_get_srq(&dev->rdma_dev_res, dct->bound_srq_handle);
                if (!srq_rm) {
                    complete_with_error(VENDOR_ERR_INV_DCT, comp_ctx);
                    pvrdma_ring_read_inc(ring);
                    wqe = pvrdma_ring_next_elem_read(ring);
                    qp->wqe_state.send_wqes_processed++;
                    continue;
                }
                comp_ctx->dc_target_srq = &srq_rm->backend_srq;
                comp_ctx->dc_recv_cq_handle = srq_rm->recv_cq_handle;
                comp_ctx->dc_src_qp = qp->qpn;
            } else if (pvrdma_opcode == PVRDMA_WR_RDMA_READ ||
                       pvrdma_opcode == PVRDMA_WR_RDMA_WRITE ||
                       pvrdma_opcode == PVRDMA_WR_RDMA_WRITE_WITH_IMM ||
                       pvrdma_opcode == PVRDMA_WR_RDMA_READ_WITH_INV) {
                comp_ctx->remote_addr = wqe->hdr.wr.rdma.remote_addr;
                comp_ctx->rkey = wqe->hdr.wr.rdma.rkey;
            } else {
                rdma_error_report("Unsupported opcode %u on DCI QP",
                                  pvrdma_opcode);
                complete_with_error(VENDOR_ERR_INV_QP_TYPE, comp_ctx);
                pvrdma_ring_read_inc(ring);
                wqe = pvrdma_ring_next_elem_read(ring);
                qp->wqe_state.send_wqes_processed++;
                continue;
            }
            sgid_idx = 0;
            sgid = rdma_rm_get_gid(&dev->rdma_dev_res, 0);
            dgid = NULL;
            dqpn = 0;
            dqkey = 0;
        } else {
            rdma_error_report("Unsupported QP type %d for send", qp->qp_type);
            complete_with_error(VENDOR_ERR_INV_QP_TYPE, comp_ctx);
            pvrdma_ring_read_inc(ring);
            qp->wqe_state.send_wqes_processed++;
            wqe = pvrdma_ring_next_elem_read(ring);
            continue;
        }

        if (wqe->hdr.num_sge > dev->dev_attr.max_sge) {
            rdma_error_report("Invalid num_sge=%d (max %d)", wqe->hdr.num_sge,
                              dev->dev_attr.max_sge);
            complete_with_error(VENDOR_ERR_INV_NUM_SGE, comp_ctx);
            pvrdma_ring_read_inc(ring);
            qp->wqe_state.send_wqes_processed++;
            wqe = pvrdma_ring_next_elem_read(ring);
            continue;
        }

        rdma_info_report(
            ">>> pvrdma_qp_ops: Before post_send: qp=%p, opcode=%u, "
            "remote_addr=0x%lx, rkey=0x%x",
            qp, pvrdma_opcode, (unsigned long)comp_ctx->remote_addr,
            comp_ctx->rkey);

        {
            PVRDMAQPStats *qp_stats = pvrdma_get_qp_stats(dev, qp_handle);
            if (qp_stats) {
                qp_stats->wqes_processed++;
                if (pvrdma_opcode < 18) {
                    qp_stats->wqes_by_opcode[pvrdma_opcode]++;
                }
            }
        }

        __atomic_fetch_add(&qp->send_in_flight, 1, __ATOMIC_ACQ_REL);

        rdma_backend_post_send(&dev->backend_dev, &qp->backend_qp, qp->qp_type,
                               (struct ibv_sge *)&wqe->sge[0], wqe->hdr.num_sge,
                               sgid_idx, sgid, dgid, dqpn, dqkey, comp_ctx);

        pvrdma_ring_read_inc(ring);
        qp->wqe_state.send_wqes_processed++;
        wqe = pvrdma_ring_next_elem_read(ring);
    }

    if (wqe) {
        schedule_wqe_processing_continuation(dev, qp_handle);
    } else {
        qp->wqe_state.send_processing_active = false;
        qp->wqe_state.send_wqes_processed = 0;
    }
}

void pvrdma_qp_recv(PVRDMADev *dev, uint32_t qp_handle)
{
    RdmaRmQP *qp;
    PvrdmaRqWqe *wqe;
    PvrdmaRing *ring;

    qp = rdma_rm_get_qp(&dev->rdma_dev_res, qp_handle);
    if (unlikely(!qp)) {
        return;
    }

    rdma_info_report(">>> pvrdma_qp_recv: qp=%p, qp->opaque=%p, &qp->opaque=%p",
                     qp, qp->opaque, &qp->opaque);
    ring = &((PvrdmaRing *)qp->opaque)[1];
    rdma_info_report(">>> pvrdma_qp_recv: Computed ring=%p", ring);

    if (qp->wqe_state.recv_processing_active) {
        rdma_info_report(">>> pvrdma_qp_recv: QP %u recv already processing, "
                         "continuation scheduled",
                         qp_handle);
        return;
    }

    wqe = pvrdma_ring_next_elem_read(ring);
    if (!wqe) {
        return;
    }

    qp->wqe_state.recv_processing_active = true;
    qp->wqe_state.recv_wqes_processed = 0;

    while (wqe && qp->wqe_state.recv_wqes_processed < WQE_BATCH_SIZE) {
        CompHandlerCtx *comp_ctx;

        comp_ctx = g_new0(CompHandlerCtx, 1);
        comp_ctx->dev = dev;
        comp_ctx->cq_handle = qp->recv_cq_handle;
        comp_ctx->qp_handle = 0;
        comp_ctx->cqe.wr_id = wqe->hdr.wr_id;
        comp_ctx->cqe.qp = qp_handle;
        comp_ctx->cqe.opcode = IBV_WC_RECV;

        if (wqe->hdr.num_sge > dev->dev_attr.max_sge) {
            rdma_error_report("Invalid num_sge=%d (max %d)", wqe->hdr.num_sge,
                              dev->dev_attr.max_sge);
            complete_with_error(VENDOR_ERR_INV_NUM_SGE, comp_ctx);
            pvrdma_ring_read_inc(ring);
            qp->wqe_state.recv_wqes_processed++;
            wqe = pvrdma_ring_next_elem_read(ring);
            continue;
        }

        rdma_backend_post_recv(&dev->backend_dev, &qp->backend_qp, qp->qp_type,
                               (struct ibv_sge *)&wqe->sge[0], wqe->hdr.num_sge,
                               comp_ctx);

        pvrdma_ring_read_inc(ring);
        qp->wqe_state.recv_wqes_processed++;
        wqe = pvrdma_ring_next_elem_read(ring);
    }

    if (wqe) {
        schedule_recv_processing_continuation(dev, qp_handle);
    } else {
        qp->wqe_state.recv_processing_active = false;
        qp->wqe_state.recv_wqes_processed = 0;
    }
}

void pvrdma_srq_recv(PVRDMADev *dev, uint32_t srq_handle)
{
    RdmaRmSRQ *srq;
    PvrdmaRqWqe *wqe;
    PvrdmaRing *ring;

    srq = rdma_rm_get_srq(&dev->rdma_dev_res, srq_handle);
    if (unlikely(!srq)) {
        return;
    }

    /* Check if already processing - if so, just mark for continuation */
    if (srq->wqe_state.processing_active) {
        /* Already processing, will be continued by idle callback */
        rdma_info_report(">>> pvrdma_srq_recv: SRQ %u already processing, "
                         "continuation scheduled",
                         srq_handle);
        return;
    }

    ring = (PvrdmaRing *)srq->opaque;

    wqe = pvrdma_ring_next_elem_read(ring);
    if (!wqe) {
        /* No WQEs to process */
        return;
    }

    /* Mark as processing */
    srq->wqe_state.processing_active = true;
    srq->wqe_state.wqes_processed = 0;

    /* Process batch of WQEs */
    while (wqe && srq->wqe_state.wqes_processed < WQE_BATCH_SIZE) {
        CompHandlerCtx *comp_ctx;

        /* Prepare CQE */
        comp_ctx = g_new0(CompHandlerCtx, 1);
        comp_ctx->dev = dev;
        comp_ctx->cq_handle = srq->recv_cq_handle;
        comp_ctx->qp_handle = 0;
        comp_ctx->cqe.wr_id = wqe->hdr.wr_id;
        comp_ctx->cqe.qp = 0;
        comp_ctx->cqe.opcode = IBV_WC_RECV;

        if (wqe->hdr.num_sge > dev->dev_attr.max_sge) {
            rdma_error_report("Invalid num_sge=%d (max %d)", wqe->hdr.num_sge,
                              dev->dev_attr.max_sge);
            complete_with_error(VENDOR_ERR_INV_NUM_SGE, comp_ctx);
            pvrdma_ring_read_inc(ring);
            srq->wqe_state.wqes_processed++;
            wqe = pvrdma_ring_next_elem_read(ring);
            continue;
        }

        /* Track WQE processing */
        {
            PVRDMAQPStats *srq_stats = pvrdma_get_qp_stats(dev, srq_handle);
            if (srq_stats) {
                srq_stats->wqes_processed++;
                /* SRQ RECV opcode is implicit */
                srq_stats->wqes_by_opcode[PVRDMA_WR_SEND]++;
            }
        }

        rdma_backend_post_srq_recv(&dev->backend_dev, &srq->backend_srq,
                                   (struct ibv_sge *)&wqe->sge[0],
                                   wqe->hdr.num_sge, comp_ctx);

        pvrdma_ring_read_inc(ring);
        srq->wqe_state.wqes_processed++;
        wqe = pvrdma_ring_next_elem_read(ring);
    }

    /* Track continuation */
    {
        PVRDMAQPStats *srq_stats = pvrdma_get_qp_stats(dev, srq_handle);
        if (srq_stats && wqe) {
            srq_stats->continuations++;
        }
    }

    /* If more WQEs remain, schedule continuation */
    if (wqe) {
        schedule_srq_recv_processing_continuation(dev, srq_handle);
    } else {
        /* All WQEs processed, clear processing flag */
        srq->wqe_state.processing_active = false;
        srq->wqe_state.wqes_processed = 0;
    }
}

void pvrdma_cq_poll(RdmaDeviceResources *dev_res, uint32_t cq_handle)
{
    RdmaRmCQ *cq;

    cq = rdma_rm_get_cq(dev_res, cq_handle);
    if (!cq) {
        return;
    }

    rdma_backend_poll_cq(dev_res, &cq->backend_cq);
}
