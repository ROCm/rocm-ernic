/*
 * RDMA Backend: None
 *
 * Minimal backend that provides stub implementations.
 * This is the default when no hardware is available.
 *
 * Copyright (C) 2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "rdma_backend_ops.h"
#include "rdma_backend_defs.h"
#include "rdma_utils.h"
#include <errno.h>
#include <string.h>

/* Init/Fini */
static int none_init(RdmaBackendDev *backend_dev, const char *config)
{
    rdma_info_report("None backend: No hardware required");
    return 0;
}

static void none_fini(RdmaBackendDev *backend_dev)
{
    /* Nothing to cleanup */
}

/* Query operations - return defaults */
static int none_query_port(RdmaBackendDev *backend_dev, 
                           struct ibv_port_attr *attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->state = 4;  /* IBV_PORT_ACTIVE */
    attr->max_mtu = 5;  /* IBV_MTU_4096 */
    attr->active_mtu = 3;  /* IBV_MTU_1024 */
    attr->gid_tbl_len = 1;
    attr->port_cap_flags = (1 << 16);  /* IBV_PORT_CM_SUP */
    attr->max_msg_sz = 0x80000000;
    attr->pkey_tbl_len = 1;
    attr->active_width = 1;
    attr->active_speed = 1;
    return 0;
}

static int none_query_device(RdmaBackendDev *backend_dev,
                             struct ibv_device_attr *attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->max_qp = 256;
    attr->max_qp_wr = 256;
    attr->max_sge = 32;
    attr->max_cq = 256;
    attr->max_cqe = 4096;
    attr->max_mr = 256;
    attr->max_pd = 256;
    return 0;
}

/* PD operations - stub implementations */
static int none_create_pd(RdmaBackendDev *backend_dev, RdmaBackendPD *pd)
{
    pd->ibpd = NULL;  /* No actual PD */
    return 0;
}

static void none_destroy_pd(RdmaBackendPD *pd)
{
    /* Nothing to free */
}

/* MR operations - stub implementations */
static int none_create_mr(RdmaBackendMR *mr, RdmaBackendPD *pd,
                          void *addr, size_t length,
                          uint64_t guest_start, int access)
{
    mr->ibpd = NULL;
    mr->ibmr = NULL;
    return 0;
}

static void none_destroy_mr(RdmaBackendMR *mr)
{
    /* Nothing to free */
}

static uint32_t none_mr_lkey(const RdmaBackendMR *mr)
{
    return 0;  /* Fake lkey */
}

static uint32_t none_mr_rkey(const RdmaBackendMR *mr)
{
    return 0;  /* Fake rkey */
}

/* CQ operations - stub implementations */
static int none_create_cq(RdmaBackendDev *backend_dev, RdmaBackendCQ *cq, 
                          int cqe)
{
    cq->backend_dev = backend_dev;
    cq->ibcq = NULL;
    return 0;
}

static void none_destroy_cq(RdmaBackendCQ *cq)
{
    /* Nothing to free */
}

static void none_poll_cq(RdmaDeviceResources *rdma_dev_res, RdmaBackendCQ *cq)
{
    /* No completions */
}

/* QP operations - stub implementations */
static int none_create_qp(RdmaBackendQP *qp, uint8_t qp_type,
                          RdmaBackendPD *pd, RdmaBackendCQ *scq, 
                          RdmaBackendCQ *rcq, RdmaBackendSRQ *srq,
                          uint32_t max_send_wr, uint32_t max_recv_wr,
                          uint32_t max_send_sge, uint32_t max_recv_sge)
{
    qp->ibpd = NULL;
    qp->ibqp = NULL;
    qp->sgid_idx = 0;
    return 0;
}

static void none_destroy_qp(RdmaBackendQP *qp, RdmaDeviceResources *dev_res)
{
    /* Nothing to free */
}

static uint32_t none_qpn(const RdmaBackendQP *qp)
{
    return 1;  /* Fake QPN */
}

/* QP state transitions - stub implementations */
static int none_qp_state_init(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                              uint8_t qp_type, uint32_t qkey)
{
    return 0;
}

static int none_qp_state_rtr(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                             uint8_t qp_type, uint8_t sgid_idx,
                             union ibv_gid *dgid, uint32_t dqpn,
                             uint32_t rq_psn, uint32_t qkey, bool qkey_set)
{
    return 0;
}

static int none_qp_state_rts(RdmaBackendQP *qp, uint8_t qp_type,
                             uint32_t sq_psn, uint32_t qkey, bool qkey_set)
{
    return 0;
}

static int none_query_qp(RdmaBackendQP *qp, struct ibv_qp_attr *attr,
                        int attr_mask, struct ibv_qp_init_attr *init_attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->qp_state = IBV_QPS_RTS;
    attr->cur_qp_state = IBV_QPS_RTS;
    attr->path_mtu = IBV_MTU_1024;
    attr->qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    
    if (init_attr) {
        memset(init_attr, 0, sizeof(*init_attr));
    }
    return 0;
}

/* Data path - stub implementations */
static void none_post_send(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                           uint8_t qp_type, struct ibv_sge *sge, 
                           uint32_t num_sge, uint8_t sgid_idx, 
                           union ibv_gid *sgid, union ibv_gid *dgid,
                           uint32_t dqpn, uint32_t dqkey, void *ctx)
{
    /* No-op: send would go nowhere */
}

static void none_post_recv(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                           uint8_t qp_type, struct ibv_sge *sge, 
                           uint32_t num_sge, void *ctx)
{
    /* No-op: no data to receive */
}

/* GID management - stub implementations */
static int none_add_gid(RdmaBackendDev *backend_dev, const char *ifname,
                       union ibv_gid *gid)
{
    return 0;  /* Pretend success */
}

static int none_del_gid(RdmaBackendDev *backend_dev, const char *ifname,
                       int gid_idx)
{
    return 0;  /* Pretend success */
}

static int none_get_backend_gid_index(RdmaBackendDev *backend_dev, 
                                      int sgid_idx)
{
    return sgid_idx;  /* Identity mapping */
}

/* SRQ operations - not implemented for none backend */
/* These are set to NULL in the ops structure */

/**
 * Backend operations structure for "none" backend
 */
const RdmaBackendOps rdma_backend_ops_none = {
    .name = "none",
    .type = RDMA_BACKEND_TYPE_NONE,
    
    .init = none_init,
    .fini = none_fini,
    
    .query_port = none_query_port,
    .query_device = none_query_device,
    
    .create_pd = none_create_pd,
    .destroy_pd = none_destroy_pd,
    
    .create_mr = none_create_mr,
    .destroy_mr = none_destroy_mr,
    .mr_lkey = none_mr_lkey,
    .mr_rkey = none_mr_rkey,
    
    .create_cq = none_create_cq,
    .destroy_cq = none_destroy_cq,
    .poll_cq = none_poll_cq,
    
    .create_qp = none_create_qp,
    .destroy_qp = none_destroy_qp,
    .qpn = none_qpn,
    
    .qp_state_init = none_qp_state_init,
    .qp_state_rtr = none_qp_state_rtr,
    .qp_state_rts = none_qp_state_rts,
    .query_qp = none_query_qp,
    
    .post_send = none_post_send,
    .post_recv = none_post_recv,
    
    .add_gid = none_add_gid,
    .del_gid = none_del_gid,
    .get_backend_gid_index = none_get_backend_gid_index,
    
    /* SRQ operations not supported */
    .create_srq = NULL,
    .destroy_srq = NULL,
    .query_srq = NULL,
    .modify_srq = NULL,
    .post_srq_recv = NULL,
};

