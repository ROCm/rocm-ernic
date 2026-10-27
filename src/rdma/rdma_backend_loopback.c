/*
 * RDMA Backend: Loopback
 *
 * Internal RDMA loopback backend for testing without hardware.  It hands out
 * the handles the resource manager records; the ionic data path moves the
 * data.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "rdma/rdma_backend_ops.h"
#include "ernic_limits.h"
#include "pvrdma/rdma_backend_defs.h"
#include "pvrdma/rdma_utils.h"
#include "qemu/thread.h"
#include "rocm-ernic-warnings.h"
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <glib.h>
#include <infiniband/verbs.h>

/*
 * Loopback Backend Data Structures
 *
 * The backend keeps no state for a PD, MR or CQ, only the handles rdma_rm
 * records.  The ionic data path moves the data and keeps its own state.
 */

typedef struct {
    uint32_t max_wr;
    uint32_t max_sge;
    uint32_t srq_limit;
} LoopbackSRQ;

typedef struct {
    uint32_t qpn;
    uint8_t qp_type;
    enum ibv_qp_state state;

    /* Connection info */
    uint32_t remote_qpn;

    /* Backend device reference for auto-pairing */
    RdmaBackendDev *backend_dev;
} LoopbackQP;

/* The first QPN handed out, and the lowest after wrapping, to stay clear of
 * the special QPs */
#define LOOPBACK_FIRST_QPN 100u

typedef struct {
    QemuMutex lock;

    /* Resource tracking */
    GHashTable *qps TSA_GUARDED_BY(lock); /* qpn -> LoopbackQP, the live QPs */

    /* Handle generators */
    uint32_t next_pd_handle TSA_GUARDED_BY(lock);
    uint32_t next_cq_handle TSA_GUARDED_BY(lock);
    uint32_t next_qpn TSA_GUARDED_BY(lock);
} LoopbackBackendPrivate;

/*
 * Helper Functions
 */

static LoopbackBackendPrivate *get_private(RdmaBackendDev *backend_dev)
{
    if (!backend_dev) {
        return NULL;
    }
    return (LoopbackBackendPrivate *)backend_dev->backend_private;
}

/*
 * Backend Lifecycle
 */

static int loopback_init(RdmaBackendDev *backend_dev, const char *config)
{
    LoopbackBackendPrivate *priv;

    (void)config;
    rdma_info_report("Loopback backend: Initializing internal emulation");

    priv = g_new0(LoopbackBackendPrivate, 1);

    qemu_mutex_init(&priv->lock);

    /* Nothing else can reach priv until it is published below */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    priv->qps =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);

    priv->next_pd_handle = 1;
    priv->next_cq_handle = 1;
    priv->next_qpn = LOOPBACK_FIRST_QPN;
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON

    backend_dev->backend_private = priv;

    rdma_info_report("Loopback backend: Initialized successfully");
    return 0;
}

static void loopback_fini(RdmaBackendDev *backend_dev)
{
    LoopbackBackendPrivate *priv = get_private(backend_dev);

    if (!priv) {
        return;
    }

    rdma_info_report("Loopback backend: Cleaning up");

    /* The device is done with the backend, which has no threads */
    ROCM_ERNIC_WARN_THREAD_SAFETY_OFF
    g_hash_table_destroy(priv->qps);
    ROCM_ERNIC_WARN_THREAD_SAFETY_ON

    qemu_mutex_destroy(&priv->lock);

    g_free(priv);
    backend_dev->backend_private = NULL;
}

/*
 * Query Operations
 */

static int loopback_query_device(RdmaBackendDev *backend_dev,
                                 struct ibv_device_attr *attr)
{
    (void)backend_dev;

    memset(attr, 0, sizeof(*attr));
    /* The counts size rdma_rm's tables, so they are the device's limits,
     * not literals: a table smaller than the LIF identity promises is the
     * real ceiling. A max_mr of 1024 once failed an nvme-rdma connect at
     * QID 8, which registers 128 regions per queue up front. */
    attr->max_qp = ERNIC_MAX_QP;
    attr->max_qp_wr = 1024;
    attr->max_sge = ERNIC_MAX_SGE;
    attr->max_cq = ERNIC_MAX_CQ;
    attr->max_cqe = 8192;
    attr->max_mr = ERNIC_MAX_MR;
    attr->max_pd = 1024;
    attr->max_mr_size = 0xFFFFFFFF;
    attr->atomic_cap = IBV_ATOMIC_HCA;
    attr->max_srq = ERNIC_MAX_SRQ;
    attr->max_srq_wr = 1024;
    attr->max_srq_sge = ERNIC_MAX_SRQ_SGE;
    return 0;
}

/*
 * Protection Domain Operations
 */

static int loopback_create_pd(RdmaBackendDev *backend_dev, RdmaBackendPD *pd)
{
    LoopbackBackendPrivate *priv = get_private(backend_dev);
    uint32_t handle;

    (void)pd;

    if (!priv) {
        return -EINVAL;
    }

    qemu_mutex_lock(&priv->lock);
    handle = priv->next_pd_handle++;
    qemu_mutex_unlock(&priv->lock);

    rdma_info_report("Loopback: Created PD handle %u", handle);
    return 0;
}

/*
 * Memory Region Operations
 */

static int loopback_create_mr(RdmaBackendMR *mr, RdmaBackendPD *pd, void *addr,
                              size_t length, uint64_t guest_start, int access)
{
    static uint32_t mr_counter = 1;
    uint32_t handle = mr_counter++;

    (void)pd;
    (void)addr;
    (void)guest_start;
    (void)access;

    /* Store handle in mr structure */
    mr->ibmr = (struct ibv_mr *)(uintptr_t)handle;

    rdma_info_report("Loopback: Created MR handle %u, len=%zu", handle, length);
    return 0;
}

static void loopback_destroy_mr(RdmaBackendMR *mr)
{
    (void)mr;

    rdma_info_report("Loopback: Destroyed MR");
}

static uint32_t loopback_mr_lkey(const RdmaBackendMR *mr)
{
    uint32_t handle = (uint32_t)(uintptr_t)mr->ibmr;
    rdma_info_report(">>> loopback_mr_lkey: mr=%p, ibmr=%p, handle=%u",
                     (const void *)mr, (void *)mr->ibmr, handle);
    return handle; /* lkey = handle */
}

/*
 * Completion Queue Operations
 */

static int loopback_create_cq(RdmaBackendDev *backend_dev, RdmaBackendCQ *cq,
                              uint32_t cqe)
{
    LoopbackBackendPrivate *priv = get_private(backend_dev);
    uint32_t handle;

    if (!priv) {
        return -EINVAL;
    }

    qemu_mutex_lock(&priv->lock);
    handle = priv->next_cq_handle++;
    qemu_mutex_unlock(&priv->lock);

    cq->backend_dev = backend_dev;
    cq->ibcq = (struct ibv_cq *)(uintptr_t)handle;

    rdma_info_report("Loopback: Created CQ handle %u with %" PRIu32 " entries",
                     handle, cqe);
    return 0;
}

static void loopback_destroy_cq(RdmaBackendCQ *cq)
{
    uint32_t handle = (uint32_t)(uintptr_t)cq->ibcq;
    rdma_info_report("Loopback: Destroyed CQ handle %u", handle);
}

/*
 * Queue Pair Operations
 */

static int loopback_create_qp(RdmaBackendQP *qp, uint8_t qp_type,
                              RdmaBackendPD *pd, RdmaBackendCQ *scq,
                              RdmaBackendCQ *rcq, RdmaBackendSRQ *srq,
                              uint32_t max_send_wr, uint32_t max_recv_wr,
                              uint32_t max_send_sge, uint32_t max_recv_sge)
{
    LoopbackBackendPrivate *priv = get_private(scq->backend_dev);
    LoopbackQP *lqp;

    (void)pd;
    (void)rcq;
    (void)srq;
    (void)max_send_wr;
    (void)max_recv_wr;
    (void)max_send_sge;
    (void)max_recv_sge;

    if (!priv) {
        return -EINVAL;
    }

    lqp = g_new0(LoopbackQP, 1);
    lqp->qp_type = qp_type;
    lqp->state = IBV_QPS_RESET;
    lqp->backend_dev = scq->backend_dev; /* Store for auto-pairing */

    /*
     * Skip any number still in use once the counter wraps: inserting over a
     * live QP would free it while the resource manager still holds it.
     * There are far fewer live QPs than numbers, so this ends.
     */
    qemu_mutex_lock(&priv->lock);
    do {
        lqp->qpn = priv->next_qpn++;
    } while (lqp->qpn < LOOPBACK_FIRST_QPN ||
             g_hash_table_contains(priv->qps, GUINT_TO_POINTER(lqp->qpn)));
    g_hash_table_insert(priv->qps, GUINT_TO_POINTER(lqp->qpn), lqp);
    qemu_mutex_unlock(&priv->lock);

    /* Store the actual LoopbackQP pointer, not the QPN! */
    qp->ibqp = (struct ibv_qp *)lqp;

    rdma_info_report("Loopback: Created QP %u type=%d (stored lqp=%p as ibqp)",
                     lqp->qpn, qp_type, (void *)lqp);
    return 0;
}

/*
 * Forget the QP, so its memory doesn't outlive it and it can't be paired
 * after it's gone. A partner paired with it keeps naming it, as a connected
 * QP whose peer was destroyed does.
 */
static void loopback_destroy_qp(RdmaBackendQP *qp, RdmaDeviceResources *dev_res)
{
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;
    LoopbackBackendPrivate *priv;
    uint32_t qpn;

    (void)dev_res;
    if (!lqp) {
        return;
    }
    qpn = lqp->qpn;
    priv = get_private(lqp->backend_dev);
    qp->ibqp = NULL;
    if (priv) {
        /* The table's value destructor frees lqp. */
        qemu_mutex_lock(&priv->lock);
        g_hash_table_remove(priv->qps, GUINT_TO_POINTER(qpn));
        qemu_mutex_unlock(&priv->lock);
    }
    rdma_info_report("Loopback: Destroyed QP %u", qpn);
}

static uint32_t loopback_qpn(const RdmaBackendQP *qp)
{
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;
    return lqp ? lqp->qpn : 0;
}

/*
 * QP State Transitions
 */

static int loopback_qp_state_init(RdmaBackendDev *backend_dev,
                                  RdmaBackendQP *qp, uint8_t qp_type,
                                  uint32_t qkey)
{
    (void)backend_dev;
    (void)qp_type;
    (void)qkey;
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;

    if (lqp) {
        lqp->state = IBV_QPS_INIT;
        rdma_info_report("Loopback: QP %u -> INIT", lqp->qpn);
    }
    return 0;
}

static int loopback_qp_state_rtr(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                                 uint8_t qp_type, uint8_t sgid_idx,
                                 union ibv_gid *dgid, uint32_t dqpn,
                                 uint32_t rq_psn, uint32_t qkey, bool qkey_set)
{
    (void)backend_dev;
    (void)sgid_idx;
    (void)dgid;
    (void)qkey;
    (void)qkey_set;
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;

    /* DCT reaches RTR as its penultimate arming step; no remote QPN needed. */
    if (qp_type == IBV_QPT_DRIVER) {
        if (lqp) {
            lqp->state = IBV_QPS_RTR;
            rdma_info_report("Loopback: DCT QP %u -> RTR (armed)", lqp->qpn);
        }
        return 0;
    }

    if (lqp) {
        lqp->state = IBV_QPS_RTR;
        lqp->remote_qpn = dqpn;

        rdma_info_report("Loopback: QP %u -> RTR (remote_qpn=%u, rq_psn=%u)",
                         lqp->qpn, dqpn, rq_psn);
    }
    return 0;
}

/*
 * Auto-pair QPs for rdma_cm simulation
 * When a QP reaches RTS without a remote_qpn, try to pair it with another
 * unpaired QP in RTS state. This simulates rdma_cm connection establishment.
 */
static void loopback_auto_pair_qp(LoopbackBackendPrivate *priv, LoopbackQP *lqp)
{
    GHashTableIter iter;
    gpointer key, value;
    LoopbackQP *other_qp;
    uint32_t other_qpn;

    if (!priv || !lqp) {
        return;
    }

    /* Only auto-pair RC/UC QPs */
    if (lqp->qp_type != IBV_QPT_RC && lqp->qp_type != IBV_QPT_UC) {
        return;
    }

    /* If remote_qpn is set to self (self-loopback), clear it for auto-pairing
     */
    if (lqp->remote_qpn == lqp->qpn) {
        lqp->remote_qpn = 0;
    }

    /* Skip if already paired with another QP */
    if (lqp->remote_qpn != 0) {
        return;
    }

    /* Find another QP in RTS state without a remote_qpn */
    /* Prefer pairing with QPs that have similar QPNs (likely created together)
     */
    qemu_mutex_lock(&priv->lock);

    /* First pass: look for QPs with similar QPNs (within 10) */
    LoopbackQP *best_match = NULL;
    uint32_t best_qpn = 0;
    uint32_t best_distance = UINT32_MAX;

    g_hash_table_iter_init(&iter, priv->qps);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        other_qpn = GPOINTER_TO_UINT(key);
        other_qp = (LoopbackQP *)value;

        /* Skip self */
        if (other_qp == lqp || other_qpn == lqp->qpn) {
            continue;
        }

        /* Match: same type, in RTS, no remote_qpn set */
        if (other_qp->qp_type == lqp->qp_type &&
            other_qp->state == IBV_QPS_RTS && other_qp->remote_qpn == 0) {
            /* Prefer QPs with similar QPNs (created close together).
             * Unsigned throughout: as int32_t, two QPNs far enough apart
             * overflow the subtraction, and abs() can't negate INT32_MIN. */
            uint32_t distance = other_qpn > lqp->qpn ? other_qpn - lqp->qpn
                                                     : lqp->qpn - other_qpn;
            if (distance < best_distance) {
                best_match = other_qp;
                best_qpn = other_qpn;
                best_distance = distance;
            }
        }
    }

    /* If we found a match, use it */
    if (best_match) {
        other_qp = best_match;
        other_qpn = best_qpn;

        /* Pair them bidirectionally */
        lqp->remote_qpn = other_qpn;
        other_qp->remote_qpn = lqp->qpn;

        rdma_info_report("Loopback: Auto-paired QP %u <-> QP %u (simulating "
                         "rdma_cm, distance=%u)",
                         lqp->qpn, other_qpn, best_distance);
        qemu_mutex_unlock(&priv->lock);
        return;
    }

    /* No match found - QP will wait for a partner */
    qemu_mutex_unlock(&priv->lock);

    rdma_info_report("Loopback: QP %u in RTS, waiting for pairing partner",
                     lqp->qpn);
}

static int loopback_qp_state_rts(RdmaBackendQP *qp, uint8_t qp_type,
                                 uint32_t sq_psn, uint32_t qkey, bool qkey_set)
{
    (void)qkey;
    (void)qkey_set;
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;
    LoopbackBackendPrivate *priv = NULL;

    if (!lqp) {
        return 0;
    }

    lqp->state = IBV_QPS_RTS;

    /* DC QPs use per-send destination addressing; no connection pairing. */
    if (qp_type == IBV_QPT_DRIVER) {
        rdma_info_report("Loopback: DC QP %u -> RTS (sq_psn=%u, no pairing)",
                         lqp->qpn, sq_psn);
        return 0;
    }

    rdma_info_report("Loopback: QP %u -> RTS (sq_psn=%u)", lqp->qpn, sq_psn);

    /* Try to auto-pair this QP with another unpaired QP */
    if (lqp->backend_dev) {
        priv = get_private(lqp->backend_dev);
        if (priv) {
            loopback_auto_pair_qp(priv, lqp);
        }
    }

    return 0;
}

static int loopback_query_qp(RdmaBackendQP *qp, struct ibv_qp_attr *attr,
                             int attr_mask, struct ibv_qp_init_attr *init_attr)
{
    LoopbackQP *lqp = (LoopbackQP *)qp->ibqp;

    (void)attr_mask;

    memset(attr, 0, sizeof(*attr));
    if (lqp) {
        attr->qp_state = lqp->state;
        attr->cur_qp_state = lqp->state;
        attr->path_mtu = IBV_MTU_1024;
        attr->qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                                IBV_ACCESS_REMOTE_WRITE |
                                IBV_ACCESS_REMOTE_READ;
        if (lqp->remote_qpn != 0) {
            attr->dest_qp_num = lqp->remote_qpn;
        }
    } else {
        attr->qp_state = IBV_QPS_RTS;
        attr->cur_qp_state = IBV_QPS_RTS;
        attr->path_mtu = IBV_MTU_1024;
        attr->qp_access_flags =
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    }

    if (init_attr) {
        memset(init_attr, 0, sizeof(*init_attr));
    }
    return 0;
}

/*
 * Shared Receive Queue
 */
static int loopback_create_srq(RdmaBackendSRQ *srq, RdmaBackendPD *pd,
                               uint32_t max_wr, uint32_t max_sge,
                               uint32_t srq_limit)
{
    LoopbackSRQ *lsrq = g_new0(LoopbackSRQ, 1);

    (void)pd;

    lsrq->max_wr = max_wr;
    lsrq->max_sge = max_sge;
    lsrq->srq_limit = srq_limit;
    srq->ibsrq = (struct ibv_srq *)(uintptr_t)lsrq;
    rdma_info_report("Loopback: Created SRQ (max_wr=%u max_sge=%u)", max_wr,
                     max_sge);
    return 0;
}

static void loopback_destroy_srq(RdmaBackendSRQ *srq)
{
    LoopbackSRQ *lsrq = (LoopbackSRQ *)(uintptr_t)srq->ibsrq;

    if (!lsrq) {
        return;
    }
    g_free(lsrq);
    srq->ibsrq = NULL;
    rdma_info_report("Loopback: Destroyed SRQ");
}

static int loopback_query_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *attr)
{
    LoopbackSRQ *lsrq = (LoopbackSRQ *)(uintptr_t)srq->ibsrq;

    if (!lsrq || !attr) {
        return -EINVAL;
    }
    attr->max_wr = lsrq->max_wr;
    attr->max_sge = lsrq->max_sge;
    attr->srq_limit = lsrq->srq_limit;
    return 0;
}

static int loopback_modify_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *attr,
                               int mask)
{
    LoopbackSRQ *lsrq = (LoopbackSRQ *)(uintptr_t)srq->ibsrq;

    if (!lsrq || !attr) {
        return -EINVAL;
    }
    if (mask & IBV_SRQ_LIMIT) {
        lsrq->srq_limit = attr->srq_limit;
    }
    return 0;
}

/*
 * GID Management
 */

static int loopback_get_backend_gid_index(RdmaBackendDev *backend_dev,
                                          int sgid_idx)
{
    (void)backend_dev;

    return sgid_idx; /* Identity mapping */
}

/*
 * Backend Operations Structure
 */
const RdmaBackendOps rdma_backend_ops_loopback = {
    .name = "loopback",
    .type = RDMA_BACKEND_TYPE_LOOPBACK,

    .init = loopback_init,
    .fini = loopback_fini,

    .query_device = loopback_query_device,

    .create_pd = loopback_create_pd,

    .create_mr = loopback_create_mr,
    .destroy_mr = loopback_destroy_mr,
    .mr_lkey = loopback_mr_lkey,

    .create_cq = loopback_create_cq,
    .destroy_cq = loopback_destroy_cq,

    .create_qp = loopback_create_qp,
    .destroy_qp = loopback_destroy_qp,
    .qpn = loopback_qpn,

    .qp_state_init = loopback_qp_state_init,
    .qp_state_rtr = loopback_qp_state_rtr,
    .qp_state_rts = loopback_qp_state_rts,
    .query_qp = loopback_query_qp,

    .get_backend_gid_index = loopback_get_backend_gid_index,

    .create_srq = loopback_create_srq,
    .destroy_srq = loopback_destroy_srq,
    .query_srq = loopback_query_srq,
    .modify_srq = loopback_modify_srq,
};
