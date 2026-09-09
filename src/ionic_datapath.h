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

/*
 * Description of a guest-resident ring or memory region as the driver hands
 * it to us: either a direct DMA address (map_count <= 1) or the address of a
 * page table of map_count le64 page addresses.
 */
struct ionic_dp_buf_desc {
    uint64_t dma_addr;
    uint32_t map_count;
    uint8_t page_size_log2;
};

struct ionic_dp_ring_desc {
    struct ionic_dp_buf_desc buf;
    uint8_t depth_log2;
    uint8_t stride_log2;
};

/* Raised once completions have been written to a CQ. */
typedef void (*ionic_dp_cq_event_fn_t)(void *opaque, uint32_t eq_id,
                                       uint32_t cq_id);

/* Create / destroy */
struct ionic_datapath *ionic_datapath_create(vfu_ctx_t *vfu_ctx,
                                             struct ionic_eth_emu *eth_emu);
void ionic_datapath_destroy(struct ionic_datapath *dp);

void ionic_datapath_set_cq_event_cb(struct ionic_datapath *dp,
                                    ionic_dp_cq_event_fn_t fn, void *opaque);

/* Registration, driven from the admin queue handlers in ionic_adminq.c. */
void ionic_datapath_register_cq(struct ionic_datapath *dp, uint32_t cq_id,
                                uint32_t eq_id,
                                const struct ionic_dp_ring_desc *ring);
void ionic_datapath_unregister_cq(struct ionic_datapath *dp, uint32_t cq_id);

void ionic_datapath_register_qp(struct ionic_datapath *dp, uint32_t qp_id,
                                uint8_t ib_qp_type, uint32_t sq_cq_id,
                                const struct ionic_dp_ring_desc *sq,
                                uint32_t rq_cq_id,
                                const struct ionic_dp_ring_desc *rq);
void ionic_datapath_unregister_qp(struct ionic_datapath *dp, uint32_t qp_id);

/* RC/UC peer, learned from MODIFY_QP's IB_QP_DEST_QPN. */
void ionic_datapath_set_dest_qp(struct ionic_datapath *dp, uint32_t qp_id,
                                uint32_t dest_qp_id);

/*
 * MR registration.  @lkey is the driver's full mrid (index | key << 24), which
 * is exactly what userspace puts in an SGE.  @va/@length describe the region in
 * the client's address space; @buf resolves it to guest physical pages.
 */
void ionic_datapath_register_mr(struct ionic_datapath *dp, uint32_t lkey,
                                uint64_t va, uint64_t length,
                                const struct ionic_dp_buf_desc *buf);
void ionic_datapath_unregister_mr(struct ionic_datapath *dp, uint32_t lkey);

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
