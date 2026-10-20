/*
 *  RDMA device: Definitions of Backend Device functions
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

#ifndef RDMA_BACKEND_H
#define RDMA_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "rdma_backend_defs.h"

struct ibv_srq_attr;
union ibv_gid;

/* Add definition for QP0 and QP1 as there is no userspace enums for them */
enum ibv_special_qp_type {
    IBV_QPT_SMI = 0,
    IBV_QPT_GSI = 1,
};

/* Multi-backend abstraction functions */
const RdmaBackendOps *rdma_backend_get_ops(RdmaBackendType type);
RdmaBackendType rdma_backend_get_type_from_string(const char *backend_str);
const char *rdma_backend_type_to_string(RdmaBackendType type);
int rdma_backend_init_with_ops(RdmaBackendDev *backend_dev,
                               RdmaBackendType type, const char *config);
void rdma_backend_fini_with_ops(RdmaBackendDev *backend_dev);

/*
 * Opaque node-to-node transport for the ionic data path.
 *
 * ionic MRs are guest-physical page tables reached over vfio-user DMA, which
 * only the ionic data path can resolve, so the mesh carries ionic's own
 * protocol as an opaque payload rather than RDMA operations of its own.
 */
typedef void (*tcp_ionic_recv_fn)(void *opaque, uint32_t src_node,
                                  const void *buf, size_t len);

/* Header and body stay separate all the way down to writev. */
int tcp_backend_send_ionic_v(RdmaBackendDev *backend_dev, uint32_t dst_node,
                             const void *hdr, size_t hdr_len, const void *body,
                             size_t body_len);
void tcp_backend_set_ionic_recv_cb(RdmaBackendDev *backend_dev,
                                   tcp_ionic_recv_fn fn, void *opaque);

/*
 * Mesh addressing, shared with the ionic data path so there is one copy of
 * the GID-to-node rule.  Returns UINT32_MAX when the backend is not a mesh.
 */
uint32_t tcp_backend_local_node_id(RdmaBackendDev *backend_dev);
uint32_t tcp_backend_node_from_gid(RdmaBackendDev *backend_dev,
                                   const union ibv_gid *dgid);
/*
 * Claim a GID for this node, or withdraw it, and re-advertise.  What makes a
 * guest address that appears after the mesh formed resolvable by a peer.
 */
int tcp_backend_claim_gid(RdmaBackendDev *backend_dev, const union ibv_gid *gid,
                          bool add);

int rdma_backend_create_srq(RdmaBackendSRQ *srq, RdmaBackendPD *pd,
                            uint32_t max_wr, uint32_t max_sge,
                            uint32_t srq_limit);
int rdma_backend_query_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *srq_attr);
int rdma_backend_modify_srq(RdmaBackendSRQ *srq, struct ibv_srq_attr *srq_attr,
                            int srq_attr_mask);
void rdma_backend_destroy_srq(RdmaBackendSRQ *srq);

#endif
