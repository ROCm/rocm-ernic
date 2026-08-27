/*
 * ionic_datapath.h — ionic RDMA data-path emulation interface
 *
 * Copyright (C) 2025-2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_DATAPATH_H
#define IONIC_DATAPATH_H

#include <stdint.h>
#include <stdbool.h>
#include <vfio-user/libvfio-user.h>

struct ionic_eth_emu;
struct ionic_datapath;

/* Create / destroy */
struct ionic_datapath *ionic_datapath_create(vfu_ctx_t *vfu_ctx,
                                             struct ionic_eth_emu *eth_emu);
void ionic_datapath_destroy(struct ionic_datapath *dp);

/* Register QP and CQ rings (called from ionic_adminq.c after CREATE_QP/CQ). */
void ionic_datapath_register_qp(struct ionic_datapath *dp, uint32_t qid,
                                uint64_t sq_dma, uint8_t sq_depth_log2,
                                uint8_t sq_stride_log2, uint32_t sq_cq_id,
                                uint64_t rq_dma, uint8_t rq_depth_log2,
                                uint8_t rq_stride_log2, uint32_t rq_cq_id);

void ionic_datapath_register_cq(struct ionic_datapath *dp, uint32_t cq_id,
                                uint64_t dma, uint32_t depth, uint32_t eq_id);

/*
 * Set the pvrdma handle so the datapath can post sends via the backend.
 * Call this once after ionic_device_init() and pvrdma_device_realize().
 * @handle: pvrdma_handle_t (void *) from pvrdma_device_create().
 */
void ionic_datapath_set_pvrdma(struct ionic_datapath *dp, void *handle);

/*
 * Process a doorbell write from BAR2.
 * @qtype:        hardware queue type (decoded from BAR2 page offset)
 * @doorbell_val: 8-byte little-endian doorbell value
 */
void ionic_datapath_doorbell(struct ionic_datapath *dp, int qtype,
                             uint64_t doorbell_val);

#endif /* IONIC_DATAPATH_H */
