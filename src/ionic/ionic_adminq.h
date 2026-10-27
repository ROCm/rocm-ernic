/*
 * ionic_adminq.h — ionic RDMA admin queue service layer
 *
 * Services ionic_v1_admin_wqe entries posted to RDMA admin queue rings by
 * ionic_rdma.ko, dispatches to rdma_rm / rdma_backend, and posts
 * ionic_v1_cqe completions to the paired admin CQ.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_ADMINQ_H
#define IONIC_ADMINQ_H

#include <stdbool.h>
#include <stdint.h>
#include <vfio-user/libvfio-user.h>

#include "rocm_ernic_compat.h"

struct ionic_adminq_ctx;

struct ionic_adminq_ctx *ionic_adminq_create(vfu_ctx_t *vfu_ctx);
void ionic_adminq_destroy(struct ionic_adminq_ctx *ctx);

/*
 * Free every CQ, QP, MR and SRQ the admin queue created, and the PD they
 * share, from the datapath and the resource manager, as RDMA_RESET_LIF
 * requires.  Not part of ionic_adminq_destroy(): at shutdown the datapath
 * goes first, and the resource manager frees everything itself.  @ctx may be
 * NULL.
 */
void ionic_adminq_release_all(struct ionic_adminq_ctx *ctx);

/*
 * The deepest admin queue, as log2 of its entries. Its doorbell's producer
 * index and its completions' cmd_idx are 16 bits, so an entry past 2^16 is
 * one the driver can't name and the device can't report.
 */
#define IONIC_ADMINQ_MAX_DEPTH_LOG2 16u

/*
 * Register a new admin queue ring pair.
 *
 * @aq_idx:         index of this AQ (0..IONIC_AQ_COUNT-1)
 * @aq_dma:         guest PA of the AQ WQE ring
 * @aq_depth_log2:  log2 of AQ ring depth (entries = 2^aq_depth_log2)
 * @cq_dma:         guest PA of the admin CQ ring
 * @cq_depth_log2:  log2 of CQ ring depth
 *
 * Returns false, registering nothing, if aq_idx is out of range, the AQ's
 * depth is above IONIC_ADMINQ_MAX_DEPTH_LOG2, or the CQ's is above
 * IONIC_RING_MAX_DEPTH_LOG2.
 */
bool ionic_adminq_register_queue(struct ionic_adminq_ctx *ctx, int aq_idx,
                                 uint64_t aq_dma, uint8_t aq_depth_log2,
                                 uint64_t cq_dma, uint8_t cq_depth_log2,
                                 uint32_t cq_id, uint32_t eq_id);

/*
 * Called after each admin CQE is written, to raise the matching EQ event.
 */
typedef void (*ionic_adminq_cq_event_fn_t)(void *opaque, uint32_t eq_id,
                                           uint32_t cq_id);
void ionic_adminq_set_cq_event_cb(struct ionic_adminq_ctx *ctx,
                                  ionic_adminq_cq_event_fn_t fn, void *opaque);

/* Set the pvrdma handle for the resource manager (ernic_rm.h). @handle is
 * from pvrdma_device_create(). */
void ionic_adminq_set_pvrdma(struct ionic_adminq_ctx *ctx,
                             pvrdma_handle_t handle);

/* Set the counters (ernic_stats.h) to count commands into and read back for
 * STATS_VALS and QP_STATS_VALS, or NULL, which fails those two. */
struct ernic_stats;
void ionic_adminq_set_stats(struct ionic_adminq_ctx *ctx,
                            struct ernic_stats *stats);

/*
 * Give the admin queue a datapath to register CQ/QP/MR rings with, so that
 * SQ/RQ doorbells later find the guest memory these commands described.
 */
struct ionic_datapath;
void ionic_adminq_set_datapath(struct ionic_adminq_ctx *ctx,
                               struct ionic_datapath *dp);

/*
 * Give the admin queue the faults to inject (ionic_fault.h), or NULL for
 * none.  The set stays owned by the caller and must outlive the queue.
 */
struct ionic_fault_set;
void ionic_adminq_set_faults(struct ionic_adminq_ctx *ctx,
                             struct ionic_fault_set *faults);

/*
 * Update the AQ producer index from a BAR2 doorbell write.
 * Called by the datapath/eth_emu when an AQ doorbell is received.
 * @aq_idx: index of the admin queue (0..IONIC_AQ_COUNT-1)
 * @p_index: producer index from the doorbell p_index field
 */
void ionic_adminq_update_prod(struct ionic_adminq_ctx *ctx, int aq_idx,
                              uint16_t p_index);

/*
 * Poll all registered AQ rings for new WQEs and process them.
 * Called from the server main loop.
 */
void ionic_adminq_poll(struct ionic_adminq_ctx *ctx, vfu_ctx_t *vfu_ctx);

#endif /* IONIC_ADMINQ_H */
