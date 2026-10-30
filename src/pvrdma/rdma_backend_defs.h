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

#include <stdint.h>

/* Forward declare backend ops (defined in rdma_backend_ops.h) */
typedef struct RdmaBackendOps RdmaBackendOps;

/**
 * Backend Types
 * Defined here to avoid circular dependency
 */
typedef enum {
    RDMA_BACKEND_TYPE_NONE,     /* No backend - minimal stubs */
    RDMA_BACKEND_TYPE_LOOPBACK, /* Internal loopback emulation */
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
} RdmaBackendDev;

/*
 * The backend objects are the resource manager's, but only the backend that
 * created one reads the fields before backend_ops: the device it was created
 * on, a number it hands out, or a pointer to its own state for the object.
 * A backend that needs no number or state leaves those zero.
 */

typedef struct RdmaBackendPD {
    RdmaBackendDev *backend_dev;
    const struct RdmaBackendOps *backend_ops; /* For destroy dispatch */
} RdmaBackendPD;

typedef struct RdmaBackendMR {
    uint32_t handle; /* The backend's number for the MR (loopback: its lkey) */
    const struct RdmaBackendOps *backend_ops; /* For destroy dispatch */
} RdmaBackendMR;

typedef struct RdmaBackendCQ {
    RdmaBackendDev *backend_dev;
    uint32_t handle; /* The backend's number for the CQ */
    const struct RdmaBackendOps *backend_ops; /* For destroy dispatch */
} RdmaBackendCQ;

typedef struct RdmaBackendQP {
    RdmaBackendDev *backend_dev;
    void *priv;   /* The backend's state for the QP (loopback: LoopbackQP) */
    uint32_t qpn; /* The QP's number, from a backend that keeps no state */
    const struct RdmaBackendOps *backend_ops; /* For destroy dispatch */
} RdmaBackendQP;

typedef struct RdmaBackendSRQ {
    void *priv; /* The backend's state for the SRQ (LoopbackSRQ, TcpSRQ) */
    const struct RdmaBackendOps *backend_ops; /* For destroy dispatch */
} RdmaBackendSRQ;

#endif
