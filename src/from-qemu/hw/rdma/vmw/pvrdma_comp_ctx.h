/*
 * Shared completion context for PVRDMA send path and loopback backend.
 *
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PVRDMA_COMP_CTX_H
#define PVRDMA_COMP_CTX_H

#include <stdint.h>

#include "../rdma_backend_defs.h"
#include "standard-headers/rdma/vmw_pvrdma-abi.h"

typedef struct PVRDMADev PVRDMADev;

typedef struct PvrdmaCompHandlerCtx {
    PVRDMADev *dev;
    uint32_t cq_handle;
    uint32_t qp_handle;
    struct pvrdma_cqe cqe;
    uint32_t opcode;
    uint64_t remote_addr;
    uint32_t rkey;
    uint32_t imm_data; /* valid for WRITE_WITH_IMM */
    RdmaBackendSRQ *dc_target_srq;
    uint32_t dc_recv_cq_handle;
    uint32_t dc_src_qp;
} PvrdmaCompHandlerCtx;

#endif /* PVRDMA_COMP_CTX_H */
