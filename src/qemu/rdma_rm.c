/*
 * QEMU paravirtual RDMA - Resource Manager Implementation
 *
 * Copyright (C) 2018 Oracle
 * Copyright (C) 2018 Red Hat Inc
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * Authors:
 *     Yuval Shaia <yuval.shaia@oracle.com>
 *     Marcel Apfelbaum <marcel@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 *
 */

/* Minimal includes instead of qemu/osdep.h */
#include <string.h>
#include <errno.h>
#include <inttypes.h> /* For PRIu32 */

#include <glib.h> /* For GBytes and GHashTable */

#include "ernic_ib.h"
#include "qemu/bitmap.h" /* For bitmap operations */
#include "qemu/thread.h" /* For qemu_mutex_lock() */
#include "rdma_utils.h"
#include "rdma_backend.h"
#include "rdma_backend_ops.h" /* For RdmaBackendOps definition */
#include "rdma_rm.h"

/*
 * Returns 0, -EOVERFLOW if tbl_sz * res_sz doesn't fit in a size_t, or
 * -ENOMEM. On failure tbl->bitmap is NULL, so res_tbl_free() skips the
 * table.
 *
 * Every entry is addressed as tbl->tbl + (size_t)handle * res_sz with
 * handle < tbl_sz, which is at most tbl_sz * res_sz and so can't wrap once
 * that product is known to fit.
 */
static inline int res_tbl_init(const char *name, RdmaRmResTbl *tbl,
                               uint32_t tbl_sz, uint32_t res_sz)
{
    size_t tbl_bytes;

    strncpy(tbl->name, name, MAX_RM_TBL_NAME);
    tbl->name[MAX_RM_TBL_NAME - 1] = 0;
    tbl->tbl = NULL;
    tbl->bitmap = NULL;

    if (res_sz != 0 && tbl_sz > SIZE_MAX / res_sz) {
        rdma_error_report("Table %s, %" PRIu32 " entries of %" PRIu32
                          " bytes is too large",
                          tbl->name, tbl_sz, res_sz);
        return -EOVERFLOW;
    }
    tbl_bytes = (size_t)tbl_sz * res_sz;

    /* g_try_malloc() returns NULL for zero bytes, which isn't a failure. */
    tbl->tbl = g_try_malloc(tbl_bytes);
    if (!tbl->tbl && tbl_bytes != 0) {
        rdma_error_report("Table %s, failed to allocate %zu bytes", tbl->name,
                          tbl_bytes);
        return -ENOMEM;
    }

    tbl->bitmap = bitmap_new(tbl_sz);
    if (!tbl->bitmap) {
        rdma_error_report("Table %s, failed to allocate the bitmap", tbl->name);
        g_free(tbl->tbl);
        tbl->tbl = NULL;
        return -ENOMEM;
    }

    tbl->tbl_sz = tbl_sz;
    tbl->res_sz = res_sz;
    tbl->used = 0;
    qemu_mutex_init(&tbl->lock);

    return 0;
}

static inline void res_tbl_free(RdmaRmResTbl *tbl)
{
    if (!tbl->bitmap) {
        return;
    }
    qemu_mutex_destroy(&tbl->lock);
    g_free(tbl->tbl);
    tbl->tbl = NULL;
    g_free(tbl->bitmap);
    tbl->bitmap = NULL;
}

static inline void *rdma_res_tbl_get(RdmaRmResTbl *tbl, uint32_t handle)
{
    if ((handle < tbl->tbl_sz) && (test_bit(handle, tbl->bitmap))) {
        /* handle < tbl_sz, so this can't wrap; see res_tbl_init(). */
        return tbl->tbl + (size_t)handle * tbl->res_sz;
    } else {
        rdma_error_report("Table %s, invalid handle %" PRIu32, tbl->name,
                          handle);
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
                          "(tbl=%p, tbl_sz=%zu, res_sz=%zu)",
                          tbl->name, (void *)tbl, tbl->tbl_sz, tbl->res_sz);
        return NULL;
    }

    qemu_mutex_lock(&tbl->lock);

    /*
     * find_first_zero_bit() returns the size it was given when every bit is
     * set, so tbl_sz itself means "full" -- it is one past the last valid
     * handle, not a usable one. The comparison must be >=; with > the
     * full-table case falls through to the set_bit()/memset() below.
     *
     * The memset is then always out of bounds, since tbl->tbl is sized
     * tbl_sz * res_sz exactly. What set_bit() hits depends on the size:
     * bitmap_new() rounds up to whole words, so it lands past the end of
     * the bitmap only when tbl_sz is a multiple of 64, and otherwise sets
     * a bit in the allocated padding, marking a nonexistent handle as
     * taken. Every table here is currently word-aligned, but this helper
     * is generic and must stay correct for any size.
     */
    *handle = find_first_zero_bit(tbl->bitmap, tbl->tbl_sz);
    if (*handle >= tbl->tbl_sz) {
        rdma_error_report("Table %s, failed to allocate, bitmap is full",
                          tbl->name);
        qemu_mutex_unlock(&tbl->lock);
        return NULL;
    }

    set_bit(*handle, tbl->bitmap);

    tbl->used++;

    qemu_mutex_unlock(&tbl->lock);

    /* *handle < tbl_sz, so this can't wrap; see res_tbl_init(). */
    memset(tbl->tbl + (size_t)*handle * tbl->res_sz, 0, tbl->res_sz);

    return tbl->tbl + (size_t)*handle * tbl->res_sz;
}

static inline void rdma_res_tbl_dealloc(RdmaRmResTbl *tbl, uint32_t handle)
{
    qemu_mutex_lock(&tbl->lock);

    if (handle < tbl->tbl_sz) {
        clear_bit(handle, tbl->bitmap);
        tbl->used--;
    }

    qemu_mutex_unlock(&tbl->lock);
}

int rdma_rm_alloc_pd(RdmaDeviceResources *dev_res, RdmaBackendDev *backend_dev,
                     uint32_t *pd_handle)
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

/*
 * Free a PD. Nothing checks that its QPs, MRs and SRQs are gone, so the
 * caller frees them first. No backend keeps anything for a PD, so there is
 * no backend operation to undo.
 */
void rdma_rm_dealloc_pd(RdmaDeviceResources *dev_res, uint32_t pd_handle)
{
    if (rdma_rm_get_pd(dev_res, pd_handle)) {
        rdma_res_tbl_dealloc(&dev_res->pd_tbl, pd_handle);
        rdma_info_report("rdma_rm_dealloc_pd: Deallocated PD handle %u",
                         pd_handle);
    }
}

int rdma_rm_alloc_mr(RdmaDeviceResources *dev_res, uint32_t pd_handle,
                     int access_flags, uint32_t *mr_handle, uint32_t *lkey,
                     uint32_t *rkey)
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

    /*
     * Create backend MR using vtable dispatch. There is no host memory behind
     * the MR (see RdmaRmMR), so the backend gets no address or length.
     */
    if (pd->backend_pd.backend_ops && pd->backend_pd.backend_ops->create_mr) {
        ret = pd->backend_pd.backend_ops->create_mr(
            &mr->backend_mr, &pd->backend_pd, NULL, 0, 0, access_flags);
        if (ret) {
            rdma_error_report("Backend create_mr failed: %d", ret);
            ret = -EIO;
            goto out_dealloc_mr;
        }
        mr->backend_mr.backend_ops =
            pd->backend_pd.backend_ops; /* Store for destroy */
        /*
         * Prefer the backend-provided lkey so
         * guest SGE lkeys match the backend MR.
         * Fall back to mr_handle for backends that
         * return 0 (e.g. "none").
         */
        if (pd->backend_pd.backend_ops->mr_lkey) {
            uint32_t blkey =
                pd->backend_pd.backend_ops->mr_lkey(&mr->backend_mr);
            *lkey = blkey ? blkey : *mr_handle;
        } else {
            *lkey = *mr_handle;
        }
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

    *rkey = *mr_handle;

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
        rdma_res_tbl_dealloc(&dev_res->mr_tbl, mr_handle);
        rdma_info_report("rdma_rm_dealloc_mr: Deallocated MR handle %u",
                         mr_handle);
    }
}

RdmaRmCQ *rdma_rm_get_cq(RdmaDeviceResources *dev_res, uint32_t cq_handle)
{
    return rdma_res_tbl_get(&dev_res->cq_tbl, cq_handle);
}

int rdma_rm_alloc_cq(RdmaDeviceResources *dev_res, RdmaBackendDev *backend_dev,
                     uint32_t cqe, uint32_t *cq_handle)
{
    int rc;
    RdmaRmCQ *cq;

    cq = rdma_res_tbl_alloc(&dev_res->cq_tbl, cq_handle);
    if (!cq) {
        return -ENOMEM;
    }

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
        rdma_error_report("Invalid QP handle %" PRIu32, qpn);
    }

    return qp;
}

int rdma_rm_alloc_qp(RdmaDeviceResources *dev_res, uint32_t pd_handle,
                     uint8_t qp_type, uint32_t max_send_wr,
                     uint32_t max_send_sge, uint32_t send_cq_handle,
                     uint32_t max_recv_wr, uint32_t max_recv_sge,
                     uint32_t recv_cq_handle, uint32_t *qpn, uint8_t is_srq,
                     uint32_t srq_handle, RdmaRmQP **out_qp)
{
    int rc;
    RdmaRmQP *qp;
    RdmaRmCQ *scq, *rcq;
    RdmaRmPD *pd;
    RdmaRmSRQ *srq = NULL;
    uint32_t rm_qpn;
    GBytes *key;

    pd = rdma_rm_get_pd(dev_res, pd_handle);
    if (!pd) {
        return -EINVAL;
    }

    scq = rdma_rm_get_cq(dev_res, send_cq_handle);
    rcq = rdma_rm_get_cq(dev_res, recv_cq_handle);

    if (!scq || !rcq) {
        rdma_error_report("Invalid send_cqn %" PRIu32 " or recv_cqn %" PRIu32,
                          send_cq_handle, recv_cq_handle);
        return -EINVAL;
    }

    if (is_srq) {
        srq = rdma_rm_get_srq(dev_res, srq_handle);
        if (!srq) {
            rdma_error_report("Invalid srqn %" PRIu32, srq_handle);
            return -EINVAL;
        }
    }

    qp = rdma_res_tbl_alloc(&dev_res->qp_tbl, &rm_qpn);
    if (!qp) {
        return -ENOMEM;
    }

    qp->qpn = rm_qpn;
    qp->qp_state = IBV_QPS_RESET;
    qp->qp_type = qp_type;

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

    /*
     * Every lookup goes through the QPN, so two live QPs can't share one:
     * inserting the second would replace the first, which could then be
     * neither found nor freed. A handle is unique among live QPs; a
     * backend's number has to be checked.
     */
    key = g_bytes_new(qpn, sizeof(*qpn));
    if (g_hash_table_contains(dev_res->qp_hash, key)) {
        rdma_error_report("rdma_rm_alloc_qp: QPN %" PRIu32 " is already in use",
                          *qpn);
        g_bytes_unref(key);
        if (qp->backend_qp.backend_ops &&
            qp->backend_qp.backend_ops->destroy_qp) {
            qp->backend_qp.backend_ops->destroy_qp(&qp->backend_qp, dev_res);
        }
        rc = -EIO;
        goto out_dealloc_qp;
    }
    g_hash_table_insert(dev_res->qp_hash, key, qp);

    if (out_qp) {
        *out_qp = qp;
    }

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

    if (qp->qp_type == ERNIC_IB_QPT_SMI) {
        rdma_error_report("Got QP0 request");
        return -EPERM;
    } else if (qp->qp_type == ERNIC_IB_QPT_GSI) {
        return 0;
    }

    if (attr_mask & IBV_QP_STATE) {
        /* Call backend state transitions using vtable dispatch */
        if (qp->backend_qp.backend_ops) {
            if (qp_state == IBV_QPS_INIT &&
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

            if (qp_state == IBV_QPS_RTR &&
                qp->backend_qp.backend_ops->qp_state_rtr) {
                /* Get backend gid index if backend supports it */
                if (backend_dev &&
                    qp->backend_qp.backend_ops->get_backend_gid_index) {
                    int backend_idx =
                        qp->backend_qp.backend_ops->get_backend_gid_index(
                            backend_dev, sgid_idx);
                    /*
                     * Negative is the backend's error. Above UINT8_MAX can't
                     * be passed on in the uint8_t qp_state_rtr() takes, and
                     * truncating it would pick another GID.
                     */
                    if (backend_idx < 0 || backend_idx > UINT8_MAX) {
                        rdma_error_report("Failed to get backend sgid_idx for "
                                          "sgid_idx %u (got %d)",
                                          sgid_idx, backend_idx);
                        return -EIO;
                    }
                    sgid_idx = (uint8_t)backend_idx;
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

            if (qp_state == IBV_QPS_RTS &&
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
                qp_handle, (int)qp_state);
        }
    }

    /*
     * Recorded only once the backend has made the transition: a modification
     * that fails changes nothing, as in verbs, and the state recorded here is
     * the one rdma_rm_query_qp() reports.
     */
    if (attr_mask & IBV_QP_RQ_PSN) {
        qp->rq_psn = rq_psn & 0xffffffu;
    }
    if (attr_mask & IBV_QP_SQ_PSN) {
        qp->sq_psn = sq_psn & 0xffffffu;
    }
    if (attr_mask & IBV_QP_STATE) {
        qp->qp_state = qp_state;
    }

    return 0;
}

int rdma_rm_query_qp(RdmaDeviceResources *dev_res, uint32_t qp_handle,
                     struct ibv_qp_attr *attr, int attr_mask,
                     struct ibv_qp_init_attr *init_attr)
{
    RdmaRmQP *qp;

    qp = rdma_rm_get_qp(dev_res, qp_handle);
    if (!qp) {
        return -EINVAL;
    }

    /* Query backend using vtable dispatch if available */
    if (qp->backend_qp.backend_ops && qp->backend_qp.backend_ops->query_qp) {
        int ret = qp->backend_qp.backend_ops->query_qp(&qp->backend_qp, attr,
                                                       attr_mask, init_attr);
        if (!ret) {
            /*
             * The backends clear attr and fill only what they model, so the
             * PSNs have to be laid back over the top of their answer. So
             * does the state: a backend sees only the transitions to INIT,
             * RTR and RTS, so its idea of the state is stale once the QP
             * goes anywhere else, such as Error.
             */
            attr->qp_state = qp->qp_state;
            attr->cur_qp_state = qp->qp_state;
            attr->rq_psn = qp->rq_psn;
            attr->sq_psn = qp->sq_psn;
        }
        return ret;
    } else {
        /* The backend keeps no QP state, or there is no backend */
        rdma_info_report(
            "rdma_rm_query_qp: No backend query, returning local state for "
            "QP %u",
            qp_handle);
        memset(attr, 0, sizeof(*attr));
        attr->qp_state = qp->qp_state;
        attr->cur_qp_state = qp->qp_state;
        attr->path_mtu = IBV_MTU_1024;
        attr->qp_access_flags =
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
        attr->rq_psn = qp->rq_psn;
        attr->sq_psn = qp->sq_psn;

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
                      uint32_t *srq_handle)
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

    rdma_backend_destroy_srq(&srq->backend_srq);
    rdma_res_tbl_dealloc(&dev_res->srq_tbl, srq_handle);
}

static void destroy_qp_hash_key(gpointer data)
{
    g_bytes_unref(data);
}

/*
 * On failure everything rdma_rm_init() set up is released and its pointers
 * cleared, so a later rdma_rm_fini() on the same dev_res is still safe.
 */
int rdma_rm_init(RdmaDeviceResources *dev_res, struct ibv_device_attr *dev_attr)
{
    int ret;

    rdma_info_report("rdma_rm_init: ENTRY dev_res=%p", (void *)dev_res);

    /*
     * The limits come from the backend's query_device(), and the struct
     * ibv_device_attr fields are int. A negative one would turn into a huge
     * uint32_t table size.
     */
    if (dev_attr->max_pd < 0 || dev_attr->max_cq < 0 || dev_attr->max_mr < 0 ||
        dev_attr->max_qp < 0 || dev_attr->max_srq < 0) {
        rdma_error_report("rdma_rm_init: negative device limit (max_pd=%d, "
                          "max_cq=%d, max_mr=%d, max_qp=%d, max_srq=%d)",
                          dev_attr->max_pd, dev_attr->max_cq, dev_attr->max_mr,
                          dev_attr->max_qp, dev_attr->max_srq);
        return -EINVAL;
    }

    dev_res->qp_hash = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                             destroy_qp_hash_key, NULL);
    if (!dev_res->qp_hash) {
        return -ENOMEM;
    }

    ret = res_tbl_init("PD", &dev_res->pd_tbl, (uint32_t)dev_attr->max_pd,
                       sizeof(RdmaRmPD));
    if (ret < 0) {
        goto err_pd;
    }
    ret = res_tbl_init("CQ", &dev_res->cq_tbl, (uint32_t)dev_attr->max_cq,
                       sizeof(RdmaRmCQ));
    if (ret < 0) {
        goto err_cq;
    }
    ret = res_tbl_init("MR", &dev_res->mr_tbl, (uint32_t)dev_attr->max_mr,
                       sizeof(RdmaRmMR));
    if (ret < 0) {
        goto err_mr;
    }
    ret = res_tbl_init("QP", &dev_res->qp_tbl, (uint32_t)dev_attr->max_qp,
                       sizeof(RdmaRmQP));
    if (ret < 0) {
        goto err_qp;
    }
    ret = res_tbl_init("SRQ", &dev_res->srq_tbl, (uint32_t)dev_attr->max_srq,
                       sizeof(RdmaRmSRQ));
    if (ret < 0) {
        goto err_srq;
    }

    return 0;

err_srq:
    res_tbl_free(&dev_res->qp_tbl);
err_qp:
    res_tbl_free(&dev_res->mr_tbl);
err_mr:
    res_tbl_free(&dev_res->cq_tbl);
err_cq:
    res_tbl_free(&dev_res->pd_tbl);
err_pd:
    g_hash_table_destroy(dev_res->qp_hash);
    dev_res->qp_hash = NULL;
    return ret;
}

void rdma_rm_fini(RdmaDeviceResources *dev_res)
{
    res_tbl_free(&dev_res->srq_tbl);
    res_tbl_free(&dev_res->qp_tbl);
    res_tbl_free(&dev_res->mr_tbl);
    res_tbl_free(&dev_res->cq_tbl);
    res_tbl_free(&dev_res->pd_tbl);

    if (dev_res->qp_hash) {
        g_hash_table_destroy(dev_res->qp_hash);
    }
}
