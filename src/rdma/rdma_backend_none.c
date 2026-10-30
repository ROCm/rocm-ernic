/*
 * RDMA Backend: None
 *
 * Minimal backend that provides stub implementations.
 * This is the default when no hardware is available.
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
#include <infiniband/verbs.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Init/Fini */
static int none_init(RdmaBackendDev *backend_dev, const char *config)
{
    (void)backend_dev;
    (void)config;

    rdma_info_report("None backend: No hardware required");
    return 0;
}

static void none_fini(RdmaBackendDev *backend_dev)
{
    (void)backend_dev;
    /* Nothing to cleanup */
}

/*
 * The counts size rdma_rm's tables, so they are the device's limits: a table
 * smaller than the LIF identity promises is the real ceiling. No SRQs, as
 * this backend can't make one.
 */
static int none_query_device(RdmaBackendDev *backend_dev,
                             struct ibv_device_attr *attr)
{
    (void)backend_dev;

    memset(attr, 0, sizeof(*attr));
    attr->max_qp = ERNIC_MAX_QP;
    attr->max_qp_wr = 256;
    attr->max_sge = ERNIC_MAX_SGE;
    attr->max_cq = ERNIC_MAX_CQ;
    attr->max_cqe = 4096;
    attr->max_mr = ERNIC_MAX_MR;
    attr->max_pd = 256;
    return 0;
}

/* PD operations - stub implementations */
static int none_create_pd(RdmaBackendDev *backend_dev, RdmaBackendPD *pd)
{
    pd->backend_dev = backend_dev;

    return 0;
}

/* MR operations - stub implementations */
static int none_create_mr(RdmaBackendMR *mr, RdmaBackendPD *pd, int access)
{
    (void)pd;
    (void)access;

    mr->handle = 0;
    return 0;
}

static void none_destroy_mr(RdmaBackendMR *mr)
{
    (void)mr;
    /* Nothing to free */
}

static uint32_t none_mr_lkey(const RdmaBackendMR *mr)
{
    (void)mr;

    return 0; /* Fake lkey */
}

/* CQ operations - stub implementations */
static int none_create_cq(RdmaBackendDev *backend_dev, RdmaBackendCQ *cq,
                          uint32_t cqe)
{
    (void)cqe;

    cq->backend_dev = backend_dev;
    cq->handle = 0;
    return 0;
}

static void none_destroy_cq(RdmaBackendCQ *cq)
{
    (void)cq;
    /* Nothing to free */
}

/* QP operations - stub implementations */
static int none_create_qp(RdmaBackendQP *qp, uint8_t qp_type, RdmaBackendPD *pd,
                          RdmaBackendCQ *scq, RdmaBackendCQ *rcq,
                          RdmaBackendSRQ *srq, uint32_t max_send_wr,
                          uint32_t max_recv_wr, uint32_t max_send_sge,
                          uint32_t max_recv_sge)
{
    (void)qp_type;
    (void)pd;
    (void)rcq;
    (void)srq;
    (void)max_send_wr;
    (void)max_recv_wr;
    (void)max_send_sge;
    (void)max_recv_sge;

    qp->backend_dev = scq->backend_dev;
    qp->priv = NULL;
    qp->qpn = 0;
    return 0;
}

static void none_destroy_qp(RdmaBackendQP *qp, RdmaDeviceResources *dev_res)
{
    (void)qp;
    (void)dev_res;
    /* Nothing to free */
}

/* QP state transitions - stub implementations */
static int none_qp_state_init(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                              uint8_t qp_type, uint32_t qkey)
{
    (void)backend_dev;
    (void)qp;
    (void)qp_type;
    (void)qkey;

    return 0;
}

static int none_qp_state_rtr(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                             uint8_t qp_type, uint8_t sgid_idx,
                             union ibv_gid *dgid, uint32_t dqpn,
                             uint32_t rq_psn, uint32_t qkey, bool qkey_set)
{
    (void)backend_dev;
    (void)qp;
    (void)qp_type;
    (void)sgid_idx;
    (void)dgid;
    (void)dqpn;
    (void)rq_psn;
    (void)qkey;
    (void)qkey_set;

    return 0;
}

static int none_qp_state_rts(RdmaBackendQP *qp, uint8_t qp_type,
                             uint32_t sq_psn, uint32_t qkey, bool qkey_set)
{
    (void)qp;
    (void)qp_type;
    (void)sq_psn;
    (void)qkey;
    (void)qkey_set;

    return 0;
}

/* GID management - stub implementations */
static int none_get_backend_gid_index(RdmaBackendDev *backend_dev, int sgid_idx)
{
    (void)backend_dev;

    return sgid_idx; /* Identity mapping */
}

/* SRQ operations - not implemented for none backend */
/* These are set to NULL in the ops structure */

/**
 * Backend operations structure for "none" backend
 *
 * No qpn or query_qp: the backend keeps no QPs, so it has no numbers to give
 * them and no state to report. The resource manager numbers each QP by its
 * handle, which no other live QP has, and answers a query from what it
 * recorded. (A fixed QPN here once gave every QP the same number, so a
 * second QP replaced the first in the resource manager's QPN table.)
 */
const RdmaBackendOps rdma_backend_ops_none = {
    .name = "none",
    .type = RDMA_BACKEND_TYPE_NONE,

    .init = none_init,
    .fini = none_fini,

    .query_device = none_query_device,

    .create_pd = none_create_pd,

    .create_mr = none_create_mr,
    .destroy_mr = none_destroy_mr,
    .mr_lkey = none_mr_lkey,

    .create_cq = none_create_cq,
    .destroy_cq = none_destroy_cq,

    .create_qp = none_create_qp,
    .destroy_qp = none_destroy_qp,

    .qp_state_init = none_qp_state_init,
    .qp_state_rtr = none_qp_state_rtr,
    .qp_state_rts = none_qp_state_rts,

    .get_backend_gid_index = none_get_backend_gid_index,

    /* SRQ operations not supported */
    .create_srq = NULL,
    .destroy_srq = NULL,
    .query_srq = NULL,
    .modify_srq = NULL,
};
