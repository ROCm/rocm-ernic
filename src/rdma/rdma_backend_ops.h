/*
 * RDMA Backend Operations Interface
 *
 * Multi-backend abstraction layer for RDMA operations.
 * Allows supporting multiple backend types: none, loopback, tcp, etc.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef RDMA_BACKEND_OPS_H
#define RDMA_BACKEND_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ernic_mesh.h"
#include "rdma_backend_defs.h"
#include "rdma_rm_defs.h"

/* Forward declarations */
typedef struct RdmaBackendOps RdmaBackendOps;
typedef struct RdmaMeshOps RdmaMeshOps;
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
    int (*query_device)(RdmaBackendDev *backend_dev,
                        struct ibv_device_attr *attr);

    /* Protection Domain operations */
    int (*create_pd)(RdmaBackendDev *backend_dev, RdmaBackendPD *pd);

    /* Memory Region operations */
    int (*create_mr)(RdmaBackendMR *mr, RdmaBackendPD *pd, void *addr,
                     size_t length, uint64_t guest_start, int access);
    void (*destroy_mr)(RdmaBackendMR *mr);
    uint32_t (*mr_lkey)(const RdmaBackendMR *mr);

    /* Completion Queue operations */
    int (*create_cq)(RdmaBackendDev *backend_dev, RdmaBackendCQ *cq,
                     uint32_t cqe);
    void (*destroy_cq)(RdmaBackendCQ *cq);

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

    /* GID management */
    int (*get_backend_gid_index)(RdmaBackendDev *backend_dev, int sgid_idx);

    /* SRQ operations (optional - can be NULL) */
    int (*create_srq)(RdmaBackendSRQ *srq, RdmaBackendPD *pd, uint32_t max_wr,
                      uint32_t max_sge, uint32_t srq_limit);
    void (*destroy_srq)(RdmaBackendSRQ *srq);
    int (*query_srq)(RdmaBackendSRQ *srq, struct ibv_srq_attr *srq_attr);
    int (*modify_srq)(RdmaBackendSRQ *srq, struct ibv_srq_attr *srq_attr,
                      int srq_attr_mask);

    /* Node-to-node transport (optional - NULL for a backend with no peers) */
    const RdmaMeshOps *mesh;
};

/**
 * Mesh Operations
 *
 * The transport a backend with peers gives the ionic data path, which
 * carries its own protocol over it as an opaque payload: ionic MRs are
 * guest-physical page tables that only the data path can resolve, so the
 * mesh has no RDMA operations of its own.  The ernic_mesh_*() functions in
 * ernic_mesh.h call these through the device's backend, and answer for a
 * backend whose mesh is NULL themselves; each is documented there.
 *
 * set_recv_cb must not return while a receive thread is still calling the
 * callback it replaces: the data path clears its callback and then frees
 * what the callback uses.
 */
struct RdmaMeshOps {
    uint32_t (*local_node)(RdmaBackendDev *backend_dev);
    uint32_t (*node_from_gid)(RdmaBackendDev *backend_dev,
                              const union ibv_gid *dgid);
    int (*claim_gid)(RdmaBackendDev *backend_dev, const union ibv_gid *gid,
                     bool add);
    int (*sendv)(RdmaBackendDev *backend_dev, uint32_t dst_node,
                 const void *hdr, size_t hdr_len, const void *body,
                 size_t body_len, uint64_t tag, bool reply);
    bool (*has_room)(RdmaBackendDev *backend_dev, uint32_t dst_node,
                     size_t len);
    void (*cancel)(RdmaBackendDev *backend_dev, uint64_t tag, uint64_t mask);
    void (*set_recv_cb)(RdmaBackendDev *backend_dev, ernic_mesh_recv_fn fn,
                        void *opaque);
};

/* Backend implementations, registered in rdma_backend_core.c */
extern const RdmaBackendOps rdma_backend_ops_none;
extern const RdmaBackendOps rdma_backend_ops_loopback;
extern const RdmaBackendOps rdma_backend_ops_tcp;

/*
 * rdma_backend_get_ops(), rdma_backend_get_type_from_string() and
 * rdma_backend_type_to_string() are declared in rdma_backend.h, not here:
 * a second prototype trips -Wredundant-decls in every file that includes
 * both headers.
 */

/* Helper to check if backend has capability */
static inline bool rdma_backend_has_srq(const RdmaBackendOps *ops)
{
    return ops && ops->create_srq != NULL;
}

#endif /* RDMA_BACKEND_OPS_H */
