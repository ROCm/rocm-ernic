/*
 * RDMA Backend Operations Interface
 *
 * Multi-backend abstraction layer for RDMA operations.
 * Allows supporting multiple backend types: none, loopback, verbs, etc.
 *
 * Copyright (C) 2025
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef RDMA_BACKEND_OPS_H
#define RDMA_BACKEND_OPS_H

#include "rdma_backend_defs.h"
#include "rdma_rm_defs.h"

/* Forward declarations */
typedef struct RdmaBackendOps RdmaBackendOps;
typedef struct RdmaDeviceResources RdmaDeviceResources;

/* RdmaBackendType is defined in rdma_backend_defs.h */

/**
 * Backend Operations Vtable
 *
 * Each backend implements this interface to provide RDMA functionality.
 * Operations return 0 on success, negative errno on failure.
 */
struct RdmaBackendOps {
    /* Backend identification */
    const char *name;
    RdmaBackendType type;

    /* Backend lifecycle */
    int (*init)(RdmaBackendDev *backend_dev, const char *config);
    void (*fini)(RdmaBackendDev *backend_dev);

    /* Query operations */
    int (*query_port)(RdmaBackendDev *backend_dev, struct ibv_port_attr *attr);
    int (*query_device)(RdmaBackendDev *backend_dev,
                        struct ibv_device_attr *attr);

    /* Protection Domain operations */
    int (*create_pd)(RdmaBackendDev *backend_dev, RdmaBackendPD *pd);
    void (*destroy_pd)(RdmaBackendPD *pd);

    /* Memory Region operations */
    int (*create_mr)(RdmaBackendMR *mr, RdmaBackendPD *pd, void *addr,
                     size_t length, uint64_t guest_start, int access);
    void (*destroy_mr)(RdmaBackendMR *mr);
    uint32_t (*mr_lkey)(const RdmaBackendMR *mr);
    uint32_t (*mr_rkey)(const RdmaBackendMR *mr);

    /* Completion Queue operations */
    int (*create_cq)(RdmaBackendDev *backend_dev, RdmaBackendCQ *cq, int cqe);
    void (*destroy_cq)(RdmaBackendCQ *cq);
    void (*poll_cq)(RdmaDeviceResources *rdma_dev_res, RdmaBackendCQ *cq);

    /* Queue Pair operations */
    int (*create_qp)(RdmaBackendQP *qp, uint8_t qp_type, RdmaBackendPD *pd,
                     RdmaBackendCQ *scq, RdmaBackendCQ *rcq,
                     RdmaBackendSRQ *srq, uint32_t max_send_wr,
                     uint32_t max_recv_wr, uint32_t max_send_sge,
                     uint32_t max_recv_sge);
    void (*destroy_qp)(RdmaBackendQP *qp, RdmaDeviceResources *dev_res);
    uint32_t (*qpn)(const RdmaBackendQP *qp);

    /* QP state transition operations */
    int (*qp_state_init)(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                         uint8_t qp_type, uint32_t qkey);
    int (*qp_state_rtr)(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                        uint8_t qp_type, uint8_t sgid_idx, union ibv_gid *dgid,
                        uint32_t dqpn, uint32_t rq_psn, uint32_t qkey,
                        bool qkey_set);
    int (*qp_state_rts)(RdmaBackendQP *qp, uint8_t qp_type, uint32_t sq_psn,
                        uint32_t qkey, bool qkey_set);
    int (*query_qp)(RdmaBackendQP *qp, struct ibv_qp_attr *attr, int attr_mask,
                    struct ibv_qp_init_attr *init_attr);
    void (*query_remote_conn_info)(RdmaBackendQP *qp, uint64_t *remote_addr,
                                   uint32_t *rkey);

    /* Data path operations */
    void (*post_send)(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                      uint8_t qp_type, struct ibv_sge *sge, uint32_t num_sge,
                      uint8_t sgid_idx, union ibv_gid *sgid,
                      union ibv_gid *dgid, uint32_t dqpn, uint32_t dqkey,
                      void *ctx);
    void (*post_recv)(RdmaBackendDev *backend_dev, RdmaBackendQP *qp,
                      uint8_t qp_type, struct ibv_sge *sge, uint32_t num_sge,
                      void *ctx);

    /* GID management */
    int (*add_gid)(RdmaBackendDev *backend_dev, const char *ifname,
                   union ibv_gid *gid);
    int (*del_gid)(RdmaBackendDev *backend_dev, const char *ifname,
                   int gid_idx);
    int (*get_backend_gid_index)(RdmaBackendDev *backend_dev, int sgid_idx);

    /* SRQ operations (optional - can be NULL) */
    int (*create_srq)(RdmaBackendSRQ *srq, RdmaBackendPD *pd, uint32_t max_wr,
                      uint32_t max_sge, uint32_t srq_limit);
    void (*destroy_srq)(RdmaBackendSRQ *srq);
    int (*query_srq)(RdmaBackendSRQ *srq, struct ibv_srq_attr *srq_attr);
    int (*modify_srq)(RdmaBackendSRQ *srq, struct ibv_srq_attr *srq_attr,
                      int srq_attr_mask);
    void (*post_srq_recv)(RdmaBackendSRQ *srq, struct ibv_sge *sge,
                          uint32_t num_sge, void *ctx);
};

extern const RdmaBackendOps rdma_backend_ops_ofi_tcp;

/* Backend registration and management */
const RdmaBackendOps *rdma_backend_get_ops(RdmaBackendType type);
RdmaBackendType rdma_backend_get_type_from_string(const char *backend_str);
const char *rdma_backend_type_to_string(RdmaBackendType type);

/* Helper to check if backend has capability */
static inline bool rdma_backend_has_srq(const RdmaBackendOps *ops)
{
    return ops && ops->create_srq != NULL;
}

#endif /* RDMA_BACKEND_OPS_H */
