/*
 * ionic_adminq.h — ionic RDMA admin queue service layer
 *
 * Services ionic_v1_admin_wqe entries posted to RDMA admin queue rings by
 * ionic_rdma.ko, dispatches to rdma_rm / rdma_backend, and posts
 * ionic_v1_cqe completions to the paired admin CQ.
 *
 * Copyright (C) 2025-2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_ADMINQ_H
#define IONIC_ADMINQ_H

#include <stdint.h>
#include <vfio-user/libvfio-user.h>

struct ionic_adminq_ctx;

struct ionic_adminq_ctx *ionic_adminq_create(vfu_ctx_t *vfu_ctx);
void ionic_adminq_destroy(struct ionic_adminq_ctx *ctx);

/*
 * Register a new admin queue ring pair.
 *
 * @aq_idx:         index of this AQ (0..IONIC_AQ_COUNT-1)
 * @aq_dma:         guest PA of the AQ WQE ring
 * @aq_depth_log2:  log2 of AQ ring depth (entries = 2^aq_depth_log2)
 * @cq_dma:         guest PA of the admin CQ ring
 * @cq_depth_log2:  log2 of CQ ring depth
 */
void ionic_adminq_register_queue(struct ionic_adminq_ctx *ctx,
                                  int aq_idx,
                                  uint64_t aq_dma, uint8_t aq_depth_log2,
                                  uint64_t cq_dma, uint8_t cq_depth_log2);

/*
 * Set the rdma_rm / rdma_backend pointers used by opcode handlers.
 * Must be called after pvrdma_device_realize() succeeds.
 * Uses opaque void * to avoid including rdma_rm.h in the header.
 */
void ionic_adminq_set_resources(struct ionic_adminq_ctx *ctx,
                                 void *dev_res, void *backend_dev);

/* Set the pvrdma handle used by the ionic_rm_* compat wrappers.
 * @handle is a pvrdma_handle_t (void *) from pvrdma_device_create(). */
void ionic_adminq_set_pvrdma(struct ionic_adminq_ctx *ctx, void *handle);

/*
 * Update the AQ producer index from a BAR2 doorbell write.
 * Called by the datapath/eth_emu when an AQ doorbell is received.
 * @aq_idx: index of the admin queue (0..IONIC_AQ_COUNT-1)
 * @p_index: producer index from the doorbell p_index field
 */
void ionic_adminq_update_prod(struct ionic_adminq_ctx *ctx,
                               int aq_idx, uint16_t p_index);

/*
 * Poll all registered AQ rings for new WQEs and process them.
 * Called from the server main loop.
 */
void ionic_adminq_poll(struct ionic_adminq_ctx *ctx, vfu_ctx_t *vfu_ctx);

#endif /* IONIC_ADMINQ_H */
