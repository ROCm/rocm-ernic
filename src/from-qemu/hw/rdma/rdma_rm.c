/*
 * QEMU paravirtual RDMA - Resource Manager Implementation
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
#include <inttypes.h> /* For PRId64, PRIx64 */
/* #include "qapi/error.h" - Not needed for standalone */
/* #include "cpu.h" - Not needed for PVRDMA */
/* #include "monitor/monitor.h" - Not needed for standalone */

#include <sys/mman.h>      /* For munmap */
#include "qemu/compiler.h" /* For unlikely() */
#include "qemu/bitmap.h"   /* For bitmap operations */
#include "qemu/atomic.h"   /* For qatomic_set */
#include "qemu/thread.h"   /* For QEMU_LOCK_GUARD */
#include "rdma_utils.h"
#include "rdma_backend.h"
#include "rdma_backend_ops.h" /* For RdmaBackendOps definition */
#include "rdma_rm.h"

void rdma_format_device_counters(RdmaDeviceResources *dev_res, GString *buf)
{
    g_string_append_printf(buf, "\ttx               : %" PRId64 "\n",
                           dev_res->stats.tx);
    g_string_append_printf(buf, "\ttx_len           : %" PRId64 "\n",
                           dev_res->stats.tx_len);
    g_string_append_printf(buf, "\ttx_err           : %" PRId64 "\n",
                           dev_res->stats.tx_err);
    g_string_append_printf(buf, "\trx_bufs          : %" PRId64 "\n",
                           dev_res->stats.rx_bufs);
    g_string_append_printf(buf, "\trx_srq           : %" PRId64 "\n",
                           dev_res->stats.rx_srq);
    g_string_append_printf(buf, "\trx_bufs_len      : %" PRId64 "\n",
                           dev_res->stats.rx_bufs_len);
    g_string_append_printf(buf, "\trx_bufs_err      : %" PRId64 "\n",
                           dev_res->stats.rx_bufs_err);
    g_string_append_printf(buf, "\tcomps            : %" PRId64 "\n",
                           dev_res->stats.completions);
    g_string_append_printf(buf, "\tmissing_comps    : %" PRId32 "\n",
                           dev_res->stats.missing_cqe);
    g_string_append_printf(buf, "\tpoll_cq (bk)     : %" PRId64 "\n",
                           dev_res->stats.poll_cq_from_bk);
    g_string_append_printf(buf, "\tpoll_cq_ppoll_to : %" PRId64 "\n",
                           dev_res->stats.poll_cq_ppoll_to);
    g_string_append_printf(buf, "\tpoll_cq (fe)     : %" PRId64 "\n",
                           dev_res->stats.poll_cq_from_guest);
    g_string_append_printf(buf, "\tpoll_cq_empty    : %" PRId64 "\n",
                           dev_res->stats.poll_cq_from_guest_empty);
    g_string_append_printf(buf, "\tmad_tx           : %" PRId64 "\n",
                           dev_res->stats.mad_tx);
    g_string_append_printf(buf, "\tmad_tx_err       : %" PRId64 "\n",
                           dev_res->stats.mad_tx_err);
    g_string_append_printf(buf, "\tmad_rx           : %" PRId64 "\n",
                           dev_res->stats.mad_rx);
    g_string_append_printf(buf, "\tmad_rx_err       : %" PRId64 "\n",
                           dev_res->stats.mad_rx_err);
    g_string_append_printf(buf, "\tmad_rx_bufs      : %" PRId64 "\n",
                           dev_res->stats.mad_rx_bufs);
    g_string_append_printf(buf, "\tmad_rx_bufs_err  : %" PRId64 "\n",
                           dev_res->stats.mad_rx_bufs_err);
    g_string_append_printf(buf, "\tPDs              : %" PRId32 "\n",
                           dev_res->pd_tbl.used);
    g_string_append_printf(buf, "\tMRs              : %" PRId32 "\n",
                           dev_res->mr_tbl.used);
    g_string_append_printf(buf, "\tUCs              : %" PRId32 "\n",
                           dev_res->uc_tbl.used);
    g_string_append_printf(buf, "\tQPs              : %" PRId32 "\n",
                           dev_res->qp_tbl.used);
    g_string_append_printf(buf, "\tCQs              : %" PRId32 "\n",
                           dev_res->cq_tbl.used);
    g_string_append_printf(buf, "\tCEQ_CTXs         : %" PRId32 "\n",
                           dev_res->cqe_ctx_tbl.used);
}

static inline void res_tbl_init(const char *name, RdmaRmResTbl *tbl,
                                uint32_t tbl_sz, uint32_t res_sz)
{
    tbl->tbl = g_malloc(tbl_sz * res_sz);

    strncpy(tbl->name, name, MAX_RM_TBL_NAME);
    tbl->name[MAX_RM_TBL_NAME - 1] = 0;

    tbl->bitmap = bitmap_new(tbl_sz);
    tbl->tbl_sz = tbl_sz;
    tbl->res_sz = res_sz;
    tbl->used = 0;
    qemu_mutex_init(&tbl->lock);
}

static inline void res_tbl_free(RdmaRmResTbl *tbl)
{
    if (!tbl->bitmap) {
        return;
    }
    qemu_mutex_destroy(&tbl->lock);
    g_free(tbl->tbl);
    g_free(tbl->bitmap);
}

static inline void *rdma_res_tbl_get(RdmaRmResTbl *tbl, uint32_t handle)
{
    if ((handle < tbl->tbl_sz) && (test_bit(handle, tbl->bitmap))) {
        return tbl->tbl + handle * tbl->res_sz;
    } else {
        rdma_error_report("Table %s, invalid handle %d", tbl->name, handle);
        return NULL;
    }
}

static inline void *rdma_res_tbl_alloc(RdmaRmResTbl *tbl, uint32_t *handle)
{
    if (!tbl) {
        rdma_error_report("rdma_res_tbl_alloc: tbl is NULL");
        return NULL;
    }
    if (!handle) {
        rdma_error_report("rdma_res_tbl_alloc: handle pointer is NULL");
        return NULL;
    }
    if (!tbl->bitmap) {
        rdma_error_report("rdma_res_tbl_alloc: Table %s bitmap is NULL "
                          "(tbl=%p, tbl_sz=%u, res_sz=%u)",
                          tbl->name, tbl, tbl->tbl_sz, tbl->res_sz);
        return NULL;
    }

    qemu_mutex_lock(&tbl->lock);

    *handle = find_first_zero_bit(tbl->bitmap, tbl->tbl_sz);
    if (*handle > tbl->tbl_sz) {
        rdma_error_report("Table %s, failed to allocate, bitmap is full",
                          tbl->name);
        qemu_mutex_unlock(&tbl->lock);
        return NULL;
    }

    set_bit(*handle, tbl->bitmap);

    tbl->used++;

    qemu_mutex_unlock(&tbl->lock);

    memset(tbl->tbl + *handle * tbl->res_sz, 0, tbl->res_sz);

    return tbl->tbl + *handle * tbl->res_sz;
}

static inline void rdma_res_tbl_dealloc(RdmaRmResTbl *tbl, uint32_t handle)
{
    QEMU_LOCK_GUARD(&tbl->lock);

    if (handle < tbl->tbl_sz) {
        clear_bit(handle, tbl->bitmap);
        tbl->used--;
    }
}

int rdma_rm_alloc_pd(RdmaDeviceResources *dev_res, RdmaBackendDev *backend_dev,
                     uint32_t *pd_handle, uint32_t ctx_handle)
{
    RdmaRmPD *pd;
    int ret = -ENOMEM;

    pd = rdma_res_tbl_alloc(&dev_res->pd_tbl, pd_handle);
    if (!pd) {
        goto out;
    }

    /* Create backend PD using vtable dispatch */
    if (backend_dev && backend_dev->backend_ops &&
        backend_dev->backend_ops->create_pd) {
        ret = backend_dev->backend_ops->create_pd(backend_dev, &pd->backend_pd);
        if (ret) {
            rdma_error_report("Backend create_pd failed: %d", ret);
            ret = -EIO;
            goto out_tbl_dealloc;
        }
        pd->backend_pd.backend_ops =
            backend_dev->backend_ops; /* Store for destroy */
        rdma_info_report(
            "rdma_rm_alloc_pd: Created PD handle %u via backend '%s'",
            *pd_handle, backend_dev->backend_ops->name);
    } else {
        /* No backend or no create_pd operation - just allocate the PD handle */
        rdma_info_report(
            "rdma_rm_alloc_pd: No backend create_pd, allocated PD handle %u",
            *pd_handle);
        memset(&pd->backend_pd, 0, sizeof(pd->backend_pd));
    }

    pd->ctx_handle = ctx_handle;

    return 0;

out_tbl_dealloc:
    rdma_res_tbl_dealloc(&dev_res->pd_tbl, *pd_handle);

out:
    return ret;
}

RdmaRmPD *rdma_rm_get_pd(RdmaDeviceResources *dev_res, uint32_t pd_handle)
{
    return rdma_res_tbl_get(&dev_res->pd_tbl, pd_handle);
}

void rdma_rm_dealloc_pd(RdmaDeviceResources *dev_res, uint32_t pd_handle)
{
    RdmaRmPD *pd = rdma_rm_get_pd(dev_res, pd_handle);

    if (pd) {
        /* Dispatch destroy through vtable if backend exists */
        if (pd->backend_pd.backend_ops &&
            pd->backend_pd.backend_ops->destroy_pd) {
            pd->backend_pd.backend_ops->destroy_pd(&pd->backend_pd);
        }
        rdma_res_tbl_dealloc(&dev_res->pd_tbl, pd_handle);
        rdma_info_report("rdma_rm_dealloc_pd: Deallocated PD handle %u",
                         pd_handle);
    }
}

int rdma_rm_alloc_mr(RdmaDeviceResources *dev_res, uint32_t pd_handle,
                     uint64_t guest_start, uint64_t guest_length,
                     void *host_virt, int access_flags, uint32_t *mr_handle,
                     uint32_t *lkey, uint32_t *rkey)
{
    RdmaRmMR *mr;
    int ret = 0;
    RdmaRmPD *pd;

    pd = rdma_rm_get_pd(dev_res, pd_handle);
    if (!pd) {
        return -EINVAL;
    }

    mr = rdma_res_tbl_alloc(&dev_res->mr_tbl, mr_handle);
    if (!mr) {
        return -ENOMEM;
    }

    /* Set MR fields */
    mr->start = guest_start;
    mr->length = guest_length;
    if (host_virt) {
        mr->virt = host_virt;
        mr->virt += (mr->start & (PAGE_SIZE - 1));
    } else {
        mr->virt = NULL; /* Loopback backend can work without host_virt */
    }

    /* Create backend MR using vtable dispatch */
    /* This must be called even if host_virt is NULL (for loopback backend) */
    if (pd->backend_pd.backend_ops && pd->backend_pd.backend_ops->create_mr) {
        ret = pd->backend_pd.backend_ops->create_mr(
            &mr->backend_mr, &pd->backend_pd, mr->virt, mr->length, guest_start,
            access_flags);
        if (ret) {
            rdma_error_report("Backend create_mr failed: %d", ret);
            ret = -EIO;
            goto out_dealloc_mr;
        }
        mr->backend_mr.backend_ops =
            pd->backend_pd.backend_ops; /* Store for destroy */
#ifdef LEGACY_RDMA_REG_MR
        /* We keep mr_handle in lkey so send and recv get get mr ptr */
        *lkey = *mr_handle;
#else
        /* Get lkey from backend */
        if (pd->backend_pd.backend_ops->mr_lkey) {
            rdma_info_report(">>> rdma_rm_alloc_mr: Calling backend->mr_lkey, "
                             "mr_handle=%u, backend_mr.ibmr=%p",
                             *mr_handle, mr->backend_mr.ibmr);
            *lkey = pd->backend_pd.backend_ops->mr_lkey(&mr->backend_mr);
            rdma_info_report(">>> rdma_rm_alloc_mr: Backend returned lkey=0x%x",
                             *lkey);
        } else {
            *lkey = *mr_handle; /* Fallback to handle */
            rdma_info_report(">>> rdma_rm_alloc_mr: No backend->mr_lkey, using "
                             "handle as lkey=0x%x",
                             *lkey);
        }
#endif
        rdma_info_report(
            "rdma_rm_alloc_mr: Created MR handle %u via backend '%s'",
            *mr_handle, pd->backend_pd.backend_ops->name);
    } else {
        /* No backend - generate a fake lkey from handle */
        rdma_info_report("rdma_rm_alloc_mr: No backend, allocated MR handle %u",
                         *mr_handle);
        memset(&mr->backend_mr, 0, sizeof(mr->backend_mr));
        *lkey = *mr_handle; /* Use handle as lkey */
    }

    *rkey = -1;

    mr->pd_handle = pd_handle;

    return 0;

out_dealloc_mr:
    rdma_res_tbl_dealloc(&dev_res->mr_tbl, *mr_handle);

    return ret;
}

RdmaRmMR *rdma_rm_get_mr(RdmaDeviceResources *dev_res, uint32_t mr_handle)
{
    return rdma_res_tbl_get(&dev_res->mr_tbl, mr_handle);
}

void rdma_rm_dealloc_mr(RdmaDeviceResources *dev_res, uint32_t mr_handle)
{
    RdmaRmMR *mr = rdma_rm_get_mr(dev_res, mr_handle);

    if (mr) {
        /* Dispatch destroy through vtable if backend exists */
        if (mr->backend_mr.backend_ops &&
            mr->backend_mr.backend_ops->destroy_mr) {
            mr->backend_mr.backend_ops->destroy_mr(&mr->backend_mr);
        }
        if (mr->start) {
            mr->virt -= (mr->start & (PAGE_SIZE - 1));
            munmap(mr->virt, mr->length);
        }
        rdma_res_tbl_dealloc(&dev_res->mr_tbl, mr_handle);
        rdma_info_report("rdma_rm_dealloc_mr: Deallocated MR handle %u",
                         mr_handle);
    }
}

int rdma_rm_alloc_uc(RdmaDeviceResources *dev_res, uint32_t pfn,
                     uint32_t *uc_handle)
{
    RdmaRmUC *uc;

    rdma_info_report("rdma_rm_alloc_uc: ENTRY dev_res=%p, uc_tbl=%p, tbl_sz=%u",
                     dev_res, &dev_res->uc_tbl, dev_res->uc_tbl.tbl_sz);

    /* TODO: Need to make sure pfn is between bar start address and
     * bsd+RDMA_BAR2_UAR_SIZE
    if (pfn > RDMA_BAR2_UAR_SIZE) {
        rdma_error_report("pfn out of range (%d > %d)", pfn,
                          RDMA_BAR2_UAR_SIZE);
        return -ENOMEM;
    }
    */

    uc = rdma_res_tbl_alloc(&dev_res->uc_tbl, uc_handle);
    if (!uc) {
        return -ENOMEM;
    }

    return 0;
}

RdmaRmUC *rdma_rm_get_uc(RdmaDeviceResources *dev_res, uint32_t uc_handle)
{
    return rdma_res_tbl_get(&dev_res->uc_tbl, uc_handle);
}

void rdma_rm_dealloc_uc(RdmaDeviceResources *dev_res, uint32_t uc_handle)
{
    RdmaRmUC *uc = rdma_rm_get_uc(dev_res, uc_handle);

    if (uc) {
        rdma_res_tbl_dealloc(&dev_res->uc_tbl, uc_handle);
    }
}

RdmaRmCQ *rdma_rm_get_cq(RdmaDeviceResources *dev_res, uint32_t cq_handle)
{
    return rdma_res_tbl_get(&dev_res->cq_tbl, cq_handle);
}

int rdma_rm_alloc_cq(RdmaDeviceResources *dev_res, RdmaBackendDev *backend_dev,
                     uint32_t cqe, uint32_t *cq_handle, void *opaque)
{
    int rc;
    RdmaRmCQ *cq;

    cq = rdma_res_tbl_alloc(&dev_res->cq_tbl, cq_handle);
    if (!cq) {
        return -ENOMEM;
    }

    cq->opaque = opaque;
    cq->notify = CNT_CLEAR;

    /* Create backend CQ using vtable dispatch */
    if (backend_dev && backend_dev->backend_ops &&
        backend_dev->backend_ops->create_cq) {
        rc = backend_dev->backend_ops->create_cq(backend_dev, &cq->backend_cq,
                                                 cqe);
        if (rc) {
            rdma_error_report("Backend create_cq failed: %d", rc);
            rc = -EIO;
            goto out_dealloc_cq;
        }
        cq->backend_cq.backend_ops =
            backend_dev->backend_ops; /* Store for destroy */
        rdma_info_report("rdma_rm_alloc_cq: Created CQ handle %u with %u "
                         "entries via backend '%s'",
                         *cq_handle, cqe, backend_dev->backend_ops->name);
    } else {
        /* No backend - just allocate the CQ handle for tracking */
        rdma_info_report("rdma_rm_alloc_cq: No backend, allocated CQ handle %u "
                         "with %u entries",
                         *cq_handle, cqe);
        memset(&cq->backend_cq, 0, sizeof(cq->backend_cq));
    }

    return 0;

out_dealloc_cq:
    rdma_rm_dealloc_cq(dev_res, *cq_handle);

    return rc;
}

void rdma_rm_req_notify_cq(RdmaDeviceResources *dev_res, uint32_t cq_handle,
                           bool notify)
{
    RdmaRmCQ *cq;

    cq = rdma_rm_get_cq(dev_res, cq_handle);
    if (!cq) {
        return;
    }

    if (cq->notify != CNT_SET) {
        cq->notify = notify ? CNT_ARM : CNT_CLEAR;
    }
}

void rdma_rm_dealloc_cq(RdmaDeviceResources *dev_res, uint32_t cq_handle)
{
    RdmaRmCQ *cq;

    cq = rdma_rm_get_cq(dev_res, cq_handle);
    if (!cq) {
        return;
    }

    /* Dispatch destroy through vtable if backend exists */
    if (cq->backend_cq.backend_ops && cq->backend_cq.backend_ops->destroy_cq) {
        cq->backend_cq.backend_ops->destroy_cq(&cq->backend_cq);
    }

    rdma_res_tbl_dealloc(&dev_res->cq_tbl, cq_handle);
    rdma_info_report("rdma_rm_dealloc_cq: Deallocated CQ handle %u", cq_handle);
}

RdmaRmQP *rdma_rm_get_qp(RdmaDeviceResources *dev_res, uint32_t qpn)
{
    GBytes *key = g_bytes_new(&qpn, sizeof(qpn));

    RdmaRmQP *qp = g_hash_table_lookup(dev_res->qp_hash, key);

    g_bytes_unref(key);

    if (!qp) {
        rdma_error_report("Invalid QP handle %d", qpn);
    }

    return qp;
}

int rdma_rm_alloc_qp(RdmaDeviceResources *dev_res, uint32_t pd_handle,
                     uint8_t qp_type, uint32_t max_send_wr,
                     uint32_t max_send_sge, uint32_t send_cq_handle,
                     uint32_t max_recv_wr, uint32_t max_recv_sge,
                     uint32_t recv_cq_handle, void *opaque, uint32_t *qpn,
                     uint8_t is_srq, uint32_t srq_handle)
{
    int rc;
    RdmaRmQP *qp;
    RdmaRmCQ *scq, *rcq;
    RdmaRmPD *pd;
    RdmaRmSRQ *srq = NULL;
    uint32_t rm_qpn;

    pd = rdma_rm_get_pd(dev_res, pd_handle);
    if (!pd) {
        return -EINVAL;
    }

    scq = rdma_rm_get_cq(dev_res, send_cq_handle);
    rcq = rdma_rm_get_cq(dev_res, recv_cq_handle);

    if (!scq || !rcq) {
        rdma_error_report("Invalid send_cqn or recv_cqn (%d, %d)",
                          send_cq_handle, recv_cq_handle);
        return -EINVAL;
    }

    if (is_srq) {
        srq = rdma_rm_get_srq(dev_res, srq_handle);
        if (!srq) {
            rdma_error_report("Invalid srqn %d", srq_handle);
            return -EINVAL;
        }

        srq->recv_cq_handle = recv_cq_handle;
    }

    if (qp_type == IBV_QPT_GSI) {
        scq->notify = CNT_SET;
        rcq->notify = CNT_SET;
    }

    qp = rdma_res_tbl_alloc(&dev_res->qp_tbl, &rm_qpn);
    if (!qp) {
        return -ENOMEM;
    }

    qp->qpn = rm_qpn;
    qp->qp_state = IBV_QPS_RESET;
    qp->qp_type = qp_type;
    qp->send_cq_handle = send_cq_handle;
    qp->recv_cq_handle = recv_cq_handle;
    rdma_info_report(
        ">>> rdma_rm_alloc_qp: sizeof(RdmaRmQP)=%zu, sizeof(RdmaBackendQP)=%zu",
        sizeof(RdmaRmQP), sizeof(RdmaBackendQP));
    rdma_info_report(">>> rdma_rm_alloc_qp: Setting qp=%p, &qp->backend_qp=%p, "
                     "&qp->opaque=%p",
                     qp, &qp->backend_qp, &qp->opaque);
    rdma_info_report(">>> rdma_rm_alloc_qp: Offset of opaque: %zu",
                     (size_t)((char *)&qp->opaque - (char *)qp));
    rdma_info_report(">>> rdma_rm_alloc_qp: Passed opaque=%p", opaque);
    rdma_info_report(">>> rdma_rm_alloc_qp: BEFORE: qp->opaque = %p",
                     qp->opaque);
    qp->opaque = opaque;
    rdma_info_report(">>> rdma_rm_alloc_qp: AFTER:  qp->opaque = %p",
                     qp->opaque);
    qp->is_srq = is_srq;

    /* Create backend QP using vtable dispatch */
    if (pd->backend_pd.backend_ops && pd->backend_pd.backend_ops->create_qp) {
        rc = pd->backend_pd.backend_ops->create_qp(
            &qp->backend_qp, qp_type, &pd->backend_pd, &scq->backend_cq,
            &rcq->backend_cq, is_srq ? &srq->backend_srq : NULL, max_send_wr,
            max_recv_wr, max_send_sge, max_recv_sge);

        if (rc) {
            rdma_error_report("Backend create_qp failed: %d", rc);
            rc = -EIO;
            goto out_dealloc_qp;
        }

        qp->backend_qp.backend_ops =
            pd->backend_pd.backend_ops; /* Store for destroy */

        /* Get QPN from backend if available */
        if (pd->backend_pd.backend_ops->qpn) {
            *qpn = pd->backend_pd.backend_ops->qpn(&qp->backend_qp);
        } else {
            *qpn = rm_qpn; /* Fallback to local QPN */
        }
        rdma_info_report(
            "rdma_rm_alloc_qp: Created QP handle %u (QPN=%u) via backend '%s'",
            rm_qpn, *qpn, pd->backend_pd.backend_ops->name);
    } else {
        /* No backend - use local QPN and zero out backend structure */
        rdma_info_report("rdma_rm_alloc_qp: No backend, allocated QP handle %u",
                         rm_qpn);
        memset(&qp->backend_qp, 0, sizeof(qp->backend_qp));
        *qpn = rm_qpn; /* Use local QPN */
    }

    g_hash_table_insert(dev_res->qp_hash, g_bytes_new(qpn, sizeof(*qpn)), qp);

    return 0;

out_dealloc_qp:
    rdma_res_tbl_dealloc(&dev_res->qp_tbl, qp->qpn);

    return rc;
}

int rdma_rm_modify_qp(RdmaDeviceResources *dev_res, RdmaBackendDev *backend_dev,
                      uint32_t qp_handle, uint32_t attr_mask, uint8_t sgid_idx,
                      union ibv_gid *dgid, uint32_t dqpn,
                      enum ibv_qp_state qp_state, uint32_t qkey,
                      uint32_t rq_psn, uint32_t sq_psn)
{
    RdmaRmQP *qp;
    int ret;

    qp = rdma_rm_get_qp(dev_res, qp_handle);
    if (!qp) {
        return -EINVAL;
    }

    if (qp->qp_type == IBV_QPT_SMI) {
        rdma_error_report("Got QP0 request");
        return -EPERM;
    } else if (qp->qp_type == IBV_QPT_GSI) {
        return 0;
    }

    if (attr_mask & IBV_QP_STATE) {
        qp->qp_state = qp_state;

        /* Call backend state transitions using vtable dispatch */
        if (qp->backend_qp.backend_ops) {
            if (qp->qp_state == IBV_QPS_INIT &&
                qp->backend_qp.backend_ops->qp_state_init) {
                ret = qp->backend_qp.backend_ops->qp_state_init(
                    backend_dev, &qp->backend_qp, qp->qp_type, qkey);
                if (ret) {
                    rdma_error_report("Backend qp_state_init failed: %d", ret);
                    return -EIO;
                }
                rdma_info_report(
                    "rdma_rm_modify_qp: QP %u -> INIT via backend '%s'",
                    qp_handle, qp->backend_qp.backend_ops->name);
            }

            if (qp->qp_state == IBV_QPS_RTR &&
                qp->backend_qp.backend_ops->qp_state_rtr) {
                /* Get backend gid index if backend supports it */
                if (backend_dev &&
                    qp->backend_qp.backend_ops->get_backend_gid_index) {
                    sgid_idx =
                        qp->backend_qp.backend_ops->get_backend_gid_index(
                            backend_dev, sgid_idx);
                    if ((int8_t)sgid_idx <
                        0) { /* GID index 0 is valid, only negative is error */
                        rdma_error_report(
                            "Failed to get backend sgid_idx for sgid_idx %d",
                            sgid_idx);
                        return -EIO;
                    }
                }

                ret = qp->backend_qp.backend_ops->qp_state_rtr(
                    backend_dev, &qp->backend_qp, qp->qp_type, sgid_idx, dgid,
                    dqpn, rq_psn, qkey, attr_mask & IBV_QP_QKEY);
                if (ret) {
                    rdma_error_report("Backend qp_state_rtr failed: %d", ret);
                    return -EIO;
                }
                rdma_info_report(
                    "rdma_rm_modify_qp: QP %u -> RTR via backend '%s'",
                    qp_handle, qp->backend_qp.backend_ops->name);
            }

            if (qp->qp_state == IBV_QPS_RTS &&
                qp->backend_qp.backend_ops->qp_state_rts) {
                ret = qp->backend_qp.backend_ops->qp_state_rts(
                    &qp->backend_qp, qp->qp_type, sq_psn, qkey,
                    attr_mask & IBV_QP_QKEY);
                if (ret) {
                    rdma_error_report("Backend qp_state_rts failed: %d", ret);
                    return -EIO;
                }
                rdma_info_report(
                    "rdma_rm_modify_qp: QP %u -> RTS via backend '%s'",
                    qp_handle, qp->backend_qp.backend_ops->name);
            }
        } else {
            /* No backend - just track state locally */
            rdma_info_report(
                "rdma_rm_modify_qp: No backend, QP %u state transition to %d",
                qp_handle, qp_state);
        }
    }

    return 0;
}

int rdma_rm_query_qp(RdmaDeviceResources *dev_res, RdmaBackendDev *backend_dev,
                     uint32_t qp_handle, struct ibv_qp_attr *attr,
                     int attr_mask, struct ibv_qp_init_attr *init_attr)
{
    RdmaRmQP *qp;

    qp = rdma_rm_get_qp(dev_res, qp_handle);
    if (!qp) {
        return -EINVAL;
    }

    /* Query backend using vtable dispatch if available */
    if (qp->backend_qp.backend_ops && qp->backend_qp.backend_ops->query_qp) {
        return qp->backend_qp.backend_ops->query_qp(&qp->backend_qp, attr,
                                                    attr_mask, init_attr);
    } else {
        /* No backend - return basic QP attributes */
        rdma_info_report(
            "rdma_rm_query_qp: No backend, returning local state for QP %u",
            qp_handle);
        memset(attr, 0, sizeof(*attr));
        attr->qp_state = qp->qp_state;
        attr->cur_qp_state = qp->qp_state;
        attr->path_mtu = IBV_MTU_1024;
        attr->qp_access_flags =
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;

        if (init_attr) {
            memset(init_attr, 0, sizeof(*init_attr));
            init_attr->qp_type = qp->qp_type;
        }
        return 0;
    }
}

void rdma_rm_dealloc_qp(RdmaDeviceResources *dev_res, uint32_t qp_handle)
{
    RdmaRmQP *qp;
    GBytes *key;

    key = g_bytes_new(&qp_handle, sizeof(qp_handle));
    qp = g_hash_table_lookup(dev_res->qp_hash, key);
    g_hash_table_remove(dev_res->qp_hash, key);
    g_bytes_unref(key);

    if (!qp) {
        return;
    }

    /* Dispatch destroy through vtable if backend exists */
    if (qp->backend_qp.backend_ops && qp->backend_qp.backend_ops->destroy_qp) {
        qp->backend_qp.backend_ops->destroy_qp(&qp->backend_qp, dev_res);
    }

    rdma_res_tbl_dealloc(&dev_res->qp_tbl, qp->qpn);
    rdma_info_report("rdma_rm_dealloc_qp: Deallocated QP handle %u", qp_handle);
}

RdmaRmSRQ *rdma_rm_get_srq(RdmaDeviceResources *dev_res, uint32_t srq_handle)
{
    return rdma_res_tbl_get(&dev_res->srq_tbl, srq_handle);
}

int rdma_rm_alloc_srq(RdmaDeviceResources *dev_res, uint32_t pd_handle,
                      uint32_t max_wr, uint32_t max_sge, uint32_t srq_limit,
                      uint32_t *srq_handle, void *opaque)
{
    RdmaRmSRQ *srq;
    RdmaRmPD *pd;
    int rc;

    pd = rdma_rm_get_pd(dev_res, pd_handle);
    if (!pd) {
        return -EINVAL;
    }

    srq = rdma_res_tbl_alloc(&dev_res->srq_tbl, srq_handle);
    if (!srq) {
        return -ENOMEM;
    }

    rc = rdma_backend_create_srq(&srq->backend_srq, &pd->backend_pd, max_wr,
                                 max_sge, srq_limit);
    if (rc) {
        rc = -EIO;
        goto out_dealloc_srq;
    }

    srq->opaque = opaque;

    return 0;

out_dealloc_srq:
    rdma_res_tbl_dealloc(&dev_res->srq_tbl, *srq_handle);

    return rc;
}

int rdma_rm_query_srq(RdmaDeviceResources *dev_res, uint32_t srq_handle,
                      struct ibv_srq_attr *srq_attr)
{
    RdmaRmSRQ *srq;

    srq = rdma_rm_get_srq(dev_res, srq_handle);
    if (!srq) {
        return -EINVAL;
    }

    return rdma_backend_query_srq(&srq->backend_srq, srq_attr);
}

int rdma_rm_modify_srq(RdmaDeviceResources *dev_res, uint32_t srq_handle,
                       struct ibv_srq_attr *srq_attr, int srq_attr_mask)
{
    RdmaRmSRQ *srq;

    srq = rdma_rm_get_srq(dev_res, srq_handle);
    if (!srq) {
        return -EINVAL;
    }

    if ((srq_attr_mask & IBV_SRQ_LIMIT) && (srq_attr->srq_limit == 0)) {
        return -EINVAL;
    }

    if ((srq_attr_mask & IBV_SRQ_MAX_WR) && (srq_attr->max_wr == 0)) {
        return -EINVAL;
    }

    return rdma_backend_modify_srq(&srq->backend_srq, srq_attr, srq_attr_mask);
}

void rdma_rm_dealloc_srq(RdmaDeviceResources *dev_res, uint32_t srq_handle)
{
    RdmaRmSRQ *srq;

    srq = rdma_rm_get_srq(dev_res, srq_handle);
    if (!srq) {
        return;
    }

    rdma_backend_destroy_srq(&srq->backend_srq, dev_res);
    rdma_res_tbl_dealloc(&dev_res->srq_tbl, srq_handle);
}

void *rdma_rm_get_cqe_ctx(RdmaDeviceResources *dev_res, uint32_t cqe_ctx_id)
{
    void **cqe_ctx;

    cqe_ctx = rdma_res_tbl_get(&dev_res->cqe_ctx_tbl, cqe_ctx_id);
    if (!cqe_ctx) {
        return NULL;
    }

    return *cqe_ctx;
}

int rdma_rm_alloc_cqe_ctx(RdmaDeviceResources *dev_res, uint32_t *cqe_ctx_id,
                          void *ctx)
{
    void **cqe_ctx;

    cqe_ctx = rdma_res_tbl_alloc(&dev_res->cqe_ctx_tbl, cqe_ctx_id);
    if (!cqe_ctx) {
        return -ENOMEM;
    }

    *cqe_ctx = ctx;

    return 0;
}

void rdma_rm_dealloc_cqe_ctx(RdmaDeviceResources *dev_res, uint32_t cqe_ctx_id)
{
    rdma_res_tbl_dealloc(&dev_res->cqe_ctx_tbl, cqe_ctx_id);
}

int rdma_rm_add_gid(RdmaDeviceResources *dev_res, RdmaBackendDev *backend_dev,
                    const char *ifname, union ibv_gid *gid, int gid_idx)
{
    int rc;

    rc = rdma_backend_add_gid(backend_dev, ifname, gid);
    if (rc) {
        return -EINVAL;
    }

    memcpy(&dev_res->port.gid_tbl[gid_idx].gid, gid, sizeof(*gid));

    return 0;
}

int rdma_rm_del_gid(RdmaDeviceResources *dev_res, RdmaBackendDev *backend_dev,
                    const char *ifname, int gid_idx)
{
    int rc;

    if (!dev_res->port.gid_tbl[gid_idx].gid.global.interface_id) {
        return 0;
    }

    rc = rdma_backend_del_gid(backend_dev, ifname,
                              &dev_res->port.gid_tbl[gid_idx].gid);
    if (rc) {
        return -EINVAL;
    }

    memset(dev_res->port.gid_tbl[gid_idx].gid.raw, 0,
           sizeof(dev_res->port.gid_tbl[gid_idx].gid));
    dev_res->port.gid_tbl[gid_idx].backend_gid_index = -1;

    return 0;
}

int rdma_rm_get_backend_gid_index(RdmaDeviceResources *dev_res,
                                  RdmaBackendDev *backend_dev, int sgid_idx)
{
    if (unlikely(sgid_idx < 0 || sgid_idx >= MAX_PORT_GIDS)) {
        rdma_error_report("Got invalid sgid_idx %d", sgid_idx);
        return -EINVAL;
    }

    if (unlikely(dev_res->port.gid_tbl[sgid_idx].backend_gid_index == -1)) {
        dev_res->port.gid_tbl[sgid_idx].backend_gid_index =
            rdma_backend_get_gid_index(backend_dev,
                                       &dev_res->port.gid_tbl[sgid_idx].gid);
    }

    return dev_res->port.gid_tbl[sgid_idx].backend_gid_index;
}

static void destroy_qp_hash_key(gpointer data)
{
    g_bytes_unref(data);
}

static void init_ports(RdmaDeviceResources *dev_res)
{
    int i;

    memset(&dev_res->port, 0, sizeof(dev_res->port));

    dev_res->port.state = IBV_PORT_DOWN;
    for (i = 0; i < MAX_PORT_GIDS; i++) {
        dev_res->port.gid_tbl[i].backend_gid_index = -1;
    }
}

static void fini_ports(RdmaDeviceResources *dev_res,
                       RdmaBackendDev *backend_dev, const char *ifname)
{
    int i;

    dev_res->port.state = IBV_PORT_DOWN;
    for (i = 0; i < MAX_PORT_GIDS; i++) {
        rdma_rm_del_gid(dev_res, backend_dev, ifname, i);
    }
}

int rdma_rm_init(RdmaDeviceResources *dev_res, struct ibv_device_attr *dev_attr)
{
    rdma_info_report("rdma_rm_init: ENTRY dev_res=%p", dev_res);

    dev_res->qp_hash = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                             destroy_qp_hash_key, NULL);
    if (!dev_res->qp_hash) {
        return -ENOMEM;
    }

    res_tbl_init("PD", &dev_res->pd_tbl, dev_attr->max_pd, sizeof(RdmaRmPD));
    res_tbl_init("CQ", &dev_res->cq_tbl, dev_attr->max_cq, sizeof(RdmaRmCQ));
    res_tbl_init("MR", &dev_res->mr_tbl, dev_attr->max_mr, sizeof(RdmaRmMR));
    res_tbl_init("QP", &dev_res->qp_tbl, dev_attr->max_qp, sizeof(RdmaRmQP));
    res_tbl_init("CQE_CTX", &dev_res->cqe_ctx_tbl,
                 dev_attr->max_qp * dev_attr->max_qp_wr, sizeof(void *));
    res_tbl_init("UC", &dev_res->uc_tbl, MAX_UCS, sizeof(RdmaRmUC));
    rdma_info_report("rdma_rm_init: UC table initialized at %p with tbl_sz=%u",
                     &dev_res->uc_tbl, dev_res->uc_tbl.tbl_sz);
    res_tbl_init("SRQ", &dev_res->srq_tbl, dev_attr->max_srq,
                 sizeof(RdmaRmSRQ));

    init_ports(dev_res);

    qemu_mutex_init(&dev_res->lock);

    memset(&dev_res->stats, 0, sizeof(dev_res->stats));
    qatomic_set(&dev_res->stats.missing_cqe, 0);

    return 0;
}

void rdma_rm_fini(RdmaDeviceResources *dev_res, RdmaBackendDev *backend_dev,
                  const char *ifname)
{
    qemu_mutex_destroy(&dev_res->lock);

    fini_ports(dev_res, backend_dev, ifname);

    res_tbl_free(&dev_res->srq_tbl);
    res_tbl_free(&dev_res->uc_tbl);
    res_tbl_free(&dev_res->cqe_ctx_tbl);
    res_tbl_free(&dev_res->qp_tbl);
    res_tbl_free(&dev_res->mr_tbl);
    res_tbl_free(&dev_res->cq_tbl);
    res_tbl_free(&dev_res->pd_tbl);

    if (dev_res->qp_hash) {
        g_hash_table_destroy(dev_res->qp_hash);
    }
}
