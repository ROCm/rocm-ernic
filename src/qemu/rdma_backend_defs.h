/*
 *  RDMA device: Definitions of Backend Device structures
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

#ifndef RDMA_BACKEND_DEFS_H
#define RDMA_BACKEND_DEFS_H

#include <stdbool.h>
#include <stdint.h>
#include <infiniband/verbs.h>

/* Forward declare backend ops (defined in rdma_backend_ops.h) */
typedef struct RdmaBackendOps RdmaBackendOps;

/**
 * Backend Types
 * Defined here to avoid circular dependency
 */
typedef enum {
    RDMA_BACKEND_TYPE_NONE,     /* No backend - minimal stubs */
    RDMA_BACKEND_TYPE_LOOPBACK, /* Internal loopback emulation */
    RDMA_BACKEND_TYPE_VERBS,    /* libibverbs hardware backend */
    RDMA_BACKEND_TYPE_TCP,      /* TCP/IP network backend */
    RDMA_BACKEND_TYPE_NVMEOF,   /* In-process NVMe-oF controller */
    RDMA_BACKEND_TYPE_S3,       /* In-process S3-over-RDMA object store */
    RDMA_BACKEND_TYPE_MAX
} RdmaBackendType;

typedef struct RdmaDeviceResources RdmaDeviceResources;

typedef struct RdmaBackendDev {
    /* Backend abstraction */
    RdmaBackendType backend_type;
    const RdmaBackendOps *backend_ops;
    void *backend_private; /* Backend-specific data */

    /* Mesh metadata (used by TCP mesh backend) */
    bool mesh_enabled;
    uint8_t mesh_node_id;
} RdmaBackendDev;

typedef struct RdmaBackendPD {
    const struct RdmaBackendOps *backend_ops; /* For destroy dispatch */
} RdmaBackendPD;

typedef struct RdmaBackendMR {
    struct ibv_mr *ibmr;
    const struct RdmaBackendOps *backend_ops; /* For destroy dispatch */
} RdmaBackendMR;

typedef struct RdmaBackendCQ {
    RdmaBackendDev *backend_dev;
    struct ibv_cq *ibcq;
    const struct RdmaBackendOps *backend_ops; /* For destroy dispatch */
} RdmaBackendCQ;

typedef struct RdmaBackendQP {
    struct ibv_qp *ibqp;
    const struct RdmaBackendOps *backend_ops; /* For destroy dispatch */
} RdmaBackendQP;

typedef struct RdmaBackendSRQ {
    struct ibv_srq *ibsrq;
    const struct RdmaBackendOps *backend_ops; /* For destroy dispatch */
} RdmaBackendSRQ;

#endif
