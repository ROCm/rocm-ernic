/*
 * ionic_adminq_priv.h — ionic admin queue internals, for its unit tests
 *
 * The admin queue context, the opcode and command-length constants, and the
 * hardware statistics tables that ionic_adminq.c works with. Only
 * ionic_adminq.c and its unit tests include this; the rest of the device sees
 * struct ionic_adminq_ctx only through the opaque declaration in
 * ionic_adminq.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_ADMINQ_PRIV_H
#define IONIC_ADMINQ_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vfio-user/libvfio-user.h>

#include "ionic_adminq.h"
#include "ionic_datapath.h"
#include "rocm_ernic_compat.h"

/* Admin opcodes (enum ionic_v1_admin_op) */
enum ionic_v1_admin_op {
    IONIC_V1_ADMIN_NOOP = 0,
    IONIC_V1_ADMIN_CREATE_CQ = 1,
    IONIC_V1_ADMIN_CREATE_QP = 2,
    IONIC_V1_ADMIN_CREATE_MR = 3,
    IONIC_V1_ADMIN_STATS_HDRS = 4,
    IONIC_V1_ADMIN_STATS_VALS = 5,
    IONIC_V1_ADMIN_DESTROY_MR = 6,
    /* 7 reserved */
    IONIC_V1_ADMIN_DESTROY_CQ = 8,
    IONIC_V1_ADMIN_MODIFY_QP = 9,
    IONIC_V1_ADMIN_QUERY_QP = 10,
    IONIC_V1_ADMIN_DESTROY_QP = 11,
    IONIC_V1_ADMIN_DEBUG = 12,
    IONIC_V1_ADMIN_CREATE_AH = 13,
    IONIC_V1_ADMIN_QUERY_AH = 14,
    IONIC_V1_ADMIN_MODIFY_DCQCN = 15,
    IONIC_V1_ADMIN_DESTROY_AH = 16,
    IONIC_V1_ADMIN_QP_STATS_HDRS = 17,
    IONIC_V1_ADMIN_QP_STATS_VALS = 18,
    /* 19-25 reserved upstream for series in flight (RCQ and friends); the
     * SRQ opcodes were assigned explicitly at 26 and must keep those
     * numbers. See enum ionic_v1_admin_op in the kernel's ionic_fw.h. */
    IONIC_V1_ADMIN_CREATE_SRQ = 26,
    IONIC_V1_ADMIN_MODIFY_SRQ = 27,
    IONIC_V1_ADMIN_QUERY_SRQ = 28,
    IONIC_V1_ADMIN_DESTROY_SRQ = 29,
};

/*
 * QP states, as MODIFY_QP's type_state carries them. CREATE_QP packs the
 * queue type into the low nibble of its own type_state and the state above
 * it; MODIFY_QP's field is the state on its own. See enum ionic_qp_state in
 * the kernel's ionic_fw.h.
 */
enum ionic_qp_state {
    IONIC_QPS_RESET = 0,
    IONIC_QPS_INIT = 1,
    IONIC_QPS_RTR = 2,
    IONIC_QPS_RTS = 3,
    IONIC_QPS_SQD = 4,
    IONIC_QPS_SQE = 5,
    IONIC_QPS_ERR = 6,
};

/* Command body lengths, from the struct ionic_admin_* layouts that
 * ionic_adminq.c documents above each handler. */
#define IONIC_ADMIN_CREATE_AH_IN_V1_LEN   24u
#define IONIC_ADMIN_DESTROY_AH_IN_V1_LEN  4u
#define IONIC_ADMIN_CREATE_SRQ_IN_V1_LEN  42u
#define IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN 5u
/* type_state is the last field, at [62], so the body runs to 63 -- see the
 * layout above handle_create_qp_op() in ionic_adminq.c. The guard there read
 * 60, which covers id_ver at [56:59] and nothing after it, so a 60-byte body
 * still had its QP type taken from three bytes the guest never sent. */
#define IONIC_ADMIN_CREATE_QP_IN_V1_LEN 63u

/* -------------------------------------------------------------------------
 * Per-AQ ring state
 * -------------------------------------------------------------------------
 */
#define MAX_AQ     4
#define MAX_CQ_MAP 256
#define MAX_QP_MAP 256
/* Tied to the advertised ceiling, not merely equal to it today: max_srq comes
 * from IONIC_EMU_SRQ_COUNT, and a map smaller than that silently loses the
 * mappings for every SRQ past the end. */
#define MAX_SRQ_MAP IONIC_EMU_SRQ_COUNT
/*
 * One entry per region the rdma_rm table can hold.  At 256 against a
 * 1024-entry table, every MR past the 256th was created successfully and then
 * had no map entry, so DESTROY_MR could not resolve its handle and silently
 * dropped it -- leaking the resource for the lifetime of the process.  An
 * nvme-rdma connect allocates 128 regions per queue, so that ceiling was
 * reached two queues in.
 */
#define MAX_MR_MAP IONIC_MAX_MR

struct ionic_aq_ring {
    bool valid;

    /* AQ WQE ring */
    uint64_t aq_dma;   /* guest PA of WQE ring */
    uint32_t aq_depth; /* number of entries (2^depth_log2) */
    uint32_t aq_cons;  /* our consumer index (next WQE to read) */
    uint32_t aq_prod;  /* last producer index from doorbell write */

    /* Admin CQ ring */
    uint64_t cq_dma;   /* guest PA of CQE ring */
    uint32_t cq_depth; /* number of entries */
    uint32_t cq_prod;  /* our producer index */
    bool cq_color;     /* current color (flips on ring wrap) */
    uint32_t aq_id;    /* AQ id the driver knows this ring by */
    uint32_t cq_id;    /* CQ id the driver knows this ring by */
    uint32_t eq_id;    /* EQ that carries this CQ's notifications */
};

struct ionic_adminq_ctx {
    vfu_ctx_t *vfu_ctx;
    struct ionic_aq_ring aq[MAX_AQ];
    int aq_count;

    /* pvrdma handle for calling ionic_rm_* compat wrappers. */
    pvrdma_handle_t pvrdma_handle;

    /* Bytes a handler DMA'd into the command's response buffer, for the
     * completion's status_length. Reset before every dispatch; only the
     * handlers that write a response set it. */
    uint32_t resp_len;

    /* Raises an EQ event once a CQE has been written. */
    ionic_adminq_cq_event_fn_t cq_event_fn;
    void *cq_event_opaque;

    /* The driver names CQs by its own cqid; rdma_rm hands back its own
     * handles.  CREATE_QP references CQs by cqid, so keep the translation. */
    struct ionic_cq_map_entry {
        bool valid;
        uint32_t cq_id;
        uint32_t handle;
    } cq_map[MAX_CQ_MAP];

    /* Same translation for QPs: the driver's qpid vs the backend's QPN. */
    struct ionic_qp_map_entry {
        bool valid;
        uint32_t qp_id;
        uint32_t qpn;
    } qp_map[MAX_QP_MAP];

    /* And for MRs: the driver's mrid vs the backend's handle. */
    struct ionic_mr_map_entry {
        bool valid;
        uint32_t mr_id;
        uint32_t handle;
    } mr_map[MAX_MR_MAP];

    /*
     * SRQs differ from every map above: WE pick the id, not the driver.
     * The LIF identity advertises IONIC_LIF_RDMA_ALLOC_QID_SRQ, which tells
     * the driver that firmware allocates SRQ qids, so CREATE_SRQ carries the
     * driver's qid only as a hint and takes the real one out of the DMA'd
     * response (ionic_controlpath.c: srq->rq.qid = le32_to_cpu(resp_buf->id)).
     * adminq_alloc_srq() is that allocator, and the slot index below is the
     * id: see the comment there for why it is not a counter.
     */
    struct ionic_srq_map_entry {
        bool valid;
        uint32_t srq_id;
        uint32_t handle;
        /*
         * Ours, not the resource manager's. rdma_rm_modify_srq() rejects a
         * limit of 0 outright (rdma_rm.c), but 0 is the legal value meaning
         * "no low-watermark", and the guest driver accepts it -- so routing
         * every limit through the resource manager turned a valid
         * ibv_modify_srq(srq_limit=0) into an error. The resource manager
         * never does anything with the value either way: nothing reads it
         * back except QUERY_SRQ, which now reads this.
         */
        uint32_t srq_limit;
    } srq_map[MAX_SRQ_MAP];

    /* ionic allocates protection domains entirely in the driver, so no PD
     * ever reaches us.  rdma_rm still needs one, so share a single backend PD
     * across every QP. */
    bool pd_valid;
    uint32_t pd_handle;

    /* Data path that owns the SQ/RQ/CQ rings these commands describe. */
    struct ionic_datapath *dp;
};

/* -------------------------------------------------------------------------
 * Hardware statistics: the STATS_HDRS / STATS_VALS / QP_STATS_HDRS /
 * QP_STATS_VALS wire layout (see handle_stats_op() in ionic_adminq.c) and
 * the counters each table reports.
 * -------------------------------------------------------------------------
 */
#define STATS_BODY_LEN  17
#define STAT_HDR_SIZE   32
#define STAT_NAME_LEN   28
#define STAT_TYPE_SHIFT 28
#define STAT_TYPE_LE64  4u
#define STAT_VAL_SIZE   8
#define STATS_MAX_COUNT 8

struct adminq_stat {
    const char *name;
    size_t off; /* of the uint64_t in the counters struct */
};

/* The names match the --stats-file labels, so the two views line up. */
static const struct adminq_stat dev_stats[] = {
    {"commands", offsetof(struct pvrdma_dev_counters, commands)},
    {"total_bytes_sent",
     offsetof(struct pvrdma_dev_counters, total_bytes_sent)},
    {"total_bytes_received",
     offsetof(struct pvrdma_dev_counters, total_bytes_received)},
    {"total_bytes_rdma_read",
     offsetof(struct pvrdma_dev_counters, total_bytes_rdma_read)},
    {"total_bytes_rdma_write",
     offsetof(struct pvrdma_dev_counters, total_bytes_rdma_write)},
};
#define DEV_STATS_COUNT (sizeof(dev_stats) / sizeof(dev_stats[0]))

static const struct adminq_stat qp_stats[] = {
    {"doorbell_send", offsetof(struct pvrdma_qp_counters, doorbell_send)},
    {"doorbell_recv", offsetof(struct pvrdma_qp_counters, doorbell_recv)},
    {"wqes_processed", offsetof(struct pvrdma_qp_counters, wqes_processed)},
    {"cqes_posted", offsetof(struct pvrdma_qp_counters, cqes_posted)},
    {"bytes_sent", offsetof(struct pvrdma_qp_counters, bytes_sent)},
    {"bytes_received", offsetof(struct pvrdma_qp_counters, bytes_received)},
    {"bytes_rdma_read", offsetof(struct pvrdma_qp_counters, bytes_rdma_read)},
    {"bytes_rdma_write", offsetof(struct pvrdma_qp_counters, bytes_rdma_write)},
};
#define QP_STATS_COUNT (sizeof(qp_stats) / sizeof(qp_stats[0]))

_Static_assert(DEV_STATS_COUNT <= STATS_MAX_COUNT, "dev_stats too long");
_Static_assert(QP_STATS_COUNT <= STATS_MAX_COUNT, "qp_stats too long");

/*
 * Run one admin WQE: @op from its header and the @len-byte command @body
 * that follows it. Returns the completion status, 0 for success and
 * nonzero for an error, and leaves in ctx->resp_len the number of bytes
 * written to the command's response buffer.
 */
uint8_t ionic_adminq_dispatch_wqe(struct ionic_adminq_ctx *ctx, uint8_t op,
                                  const uint8_t *body, uint16_t len);

#endif /* IONIC_ADMINQ_PRIV_H */
