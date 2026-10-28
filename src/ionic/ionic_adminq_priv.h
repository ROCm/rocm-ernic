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

#include "ernic_limits.h"
#include "ernic_stats.h"
#include "ionic/ionic_adminq.h"
#include "ionic/ionic_datapath.h"
#include "rocm_ernic_compat.h"

struct ernic_dma;

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
 * QP types, as the low nibble of CREATE_QP's type_state carries them. See
 * enum ionic_qp_type in the kernel's ionic_fw.h; these are not enum
 * ibv_qp_type's numbers.
 */
enum ionic_qp_type {
    IONIC_QPT_RC = 0,
    IONIC_QPT_UC = 1,
    IONIC_QPT_RD = 2,
    IONIC_QPT_UD = 3,
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
#define IONIC_ADMIN_CREATE_MR_IN_V1_LEN   45u
#define IONIC_ADMIN_DESTROY_MR_IN_V1_LEN  4u
#define IONIC_ADMIN_DESTROY_CQ_IN_V1_LEN  4u
#define IONIC_ADMIN_DESTROY_QP_IN_V1_LEN  4u
#define IONIC_ADMIN_MODIFY_QP_IN_V1_LEN   60u
#define IONIC_ADMIN_QUERY_QP_IN_V1_LEN    34u
/* type_state is the last field, at [62], so the body runs to 63 -- see the
 * layout above handle_create_qp_op() in ionic_adminq.c. The guard there read
 * 60, which covers id_ver at [56:59] and nothing after it, so a 60-byte body
 * still had its QP type taken from three bytes the guest never sent. */
#define IONIC_ADMIN_CREATE_QP_IN_V1_LEN 63u

/* -------------------------------------------------------------------------
 * Per-AQ ring state
 * -------------------------------------------------------------------------
 */
/*
 * One entry for each queue or region the LIF identity lets the guest create,
 * from ernic_limits.h: a map smaller than that is the device's real ceiling,
 * whatever the guest was told. The CQ and QP maps were once 256 entries
 * against a promise of 65536 and 32768, so the guest's 257th CREATE failed.
 * The CQ and QP maps are searched linearly, so a much larger limit wants an
 * indexed one, as the MR map is.
 */
#define MAX_AQ      ERNIC_MAX_AQ
#define MAX_CQ_MAP  ERNIC_MAX_CQ
#define MAX_QP_MAP  ERNIC_MAX_QP
#define MAX_SRQ_MAP ERNIC_MAX_SRQ
#define MAX_MR_MAP  ERNIC_MAX_MR

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
    /* Scratch for every guest transfer. A test that builds the context
     * itself, rather than with ionic_adminq_create(), sets this too. */
    struct ernic_dma *dma;
    struct ionic_aq_ring aq[MAX_AQ];
    int aq_count;

    /* pvrdma handle, for the resource manager. */
    pvrdma_handle_t pvrdma_handle;
    /* The server's counters, or NULL */
    struct ernic_stats *stats;

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

    /*
     * Same translation for QPs: the driver's qpid vs the backend's QPN. The
     * entry also keeps the destination the last successful MODIFY_QP with
     * IBV_QP_DEST_QPN gave, as the driver named it, for QUERY_QP to report.
     */
    struct ionic_qp_map_entry {
        bool valid;
        uint32_t qp_id;
        uint32_t qpn;
        bool dest_set;
        uint32_t dest_qp_id;
    } qp_map[MAX_QP_MAP];

    /* And for MRs: the driver's mrid vs the backend's handle. Indexed by
     * ionic_mr_slot(), so the entry stays the MR's whatever key byte fast
     * registration has rotated its mrid to. */
    struct ionic_mr_map_entry {
        bool valid;
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

    /* Injected faults, or NULL; owned by the server. */
    struct ionic_fault_set *faults;
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
/* The driver accepts up to 128 entries; 64 leaves room to grow. */
#define STATS_MAX_COUNT 64

struct adminq_stat {
    const char *name;
    size_t off; /* of the uint64_t in the values struct */
};

/*
 * What a VALS command reports: the ernic_stats.h counters, then the data
 * path's RDMA error counters (IONIC_DP_RDMA_COUNTERS), at the offsets the
 * HDRS table gives.
 */
struct adminq_dev_vals {
    struct ernic_dev_counters stats;
    uint64_t rdma[IONIC_DP_RDMA_COUNT];
};

struct adminq_qp_vals {
    struct ernic_qp_counters stats;
    uint64_t rdma[IONIC_DP_RDMA_COUNT];
};

#define DEV_RDMA_STAT(id, name)                     \
    {name, offsetof(struct adminq_dev_vals, rdma) + \
               (size_t)IONIC_DP_##id * sizeof(uint64_t)},
#define QP_RDMA_STAT(id, name)                     \
    {name, offsetof(struct adminq_qp_vals, rdma) + \
               (size_t)IONIC_DP_##id * sizeof(uint64_t)},

/*
 * The first five names match the --stats-file labels, so the two views line
 * up; existing readers of them see the same files as before.
 */
static const struct adminq_stat dev_stats[] = {
    {"commands", offsetof(struct adminq_dev_vals, stats.commands)},
    {"total_bytes_sent",
     offsetof(struct adminq_dev_vals, stats.total_bytes_sent)},
    {"total_bytes_received",
     offsetof(struct adminq_dev_vals, stats.total_bytes_received)},
    {"total_bytes_rdma_read",
     offsetof(struct adminq_dev_vals, stats.total_bytes_rdma_read)},
    {"total_bytes_rdma_write",
     offsetof(struct adminq_dev_vals, stats.total_bytes_rdma_write)},
    IONIC_DP_RDMA_COUNTERS(DEV_RDMA_STAT)};
#define DEV_STATS_COUNT (sizeof(dev_stats) / sizeof(dev_stats[0]))

static const struct adminq_stat qp_stats[] = {
    {"doorbell_send", offsetof(struct adminq_qp_vals, stats.doorbell_send)},
    {"doorbell_recv", offsetof(struct adminq_qp_vals, stats.doorbell_recv)},
    {"wqes_processed", offsetof(struct adminq_qp_vals, stats.wqes_processed)},
    {"cqes_posted", offsetof(struct adminq_qp_vals, stats.cqes_posted)},
    {"bytes_sent", offsetof(struct adminq_qp_vals, stats.bytes_sent)},
    {"bytes_received", offsetof(struct adminq_qp_vals, stats.bytes_received)},
    {"bytes_rdma_read", offsetof(struct adminq_qp_vals, stats.bytes_rdma_read)},
    {"bytes_rdma_write",
     offsetof(struct adminq_qp_vals, stats.bytes_rdma_write)},
    IONIC_DP_RDMA_COUNTERS(QP_RDMA_STAT)};
#define QP_STATS_COUNT (sizeof(qp_stats) / sizeof(qp_stats[0]))

#undef DEV_RDMA_STAT
#undef QP_RDMA_STAT

/* The driver keeps 27 characters of a name; a longer one would collide. */
#define RDMA_STAT_NAME_FITS(id, name)             \
    _Static_assert(sizeof(name) <= STAT_NAME_LEN, \
                   "stat name " name " is over 27 characters");
IONIC_DP_RDMA_COUNTERS(RDMA_STAT_NAME_FITS)
#undef RDMA_STAT_NAME_FITS

_Static_assert(DEV_STATS_COUNT <= STATS_MAX_COUNT, "dev_stats too long");
_Static_assert(QP_STATS_COUNT <= STATS_MAX_COUNT, "qp_stats too long");

/*
 * Run one admin WQE: @op from its header and the @len-byte command @body
 * that follows it. Returns the completion status, an enum ionic_status_code
 * (IONIC_RC_SUCCESS, or the error the guest driver logs), and leaves in
 * ctx->resp_len the number of bytes written to the command's response
 * buffer.
 */
uint8_t ionic_adminq_dispatch_wqe(struct ionic_adminq_ctx *ctx, uint8_t op,
                                  const uint8_t *body, uint16_t len);

/* An admin completion (struct ionic_v1_cqe) is CQE_SIZE bytes; these are
 * the color and error bits of its be32 qid_type_flags. */
#define CQE_SIZE      32
#define CQE_COLOR_BIT 0x01u
#define CQE_ERROR_BIT 0x02u

/*
 * Write the admin completion for the WQE at @cmd_idx into @r's CQ and
 * advance the CQ. @status is the ionic status code, 0 for success;
 * @resp_len is the number of response bytes the command wrote, reported
 * in place of the status when it succeeded.
 */
void ionic_adminq_post_cqe(const struct ionic_adminq_ctx *ctx,
                           struct ionic_aq_ring *r, uint16_t cmd_idx,
                           uint8_t cmd_op, uint8_t status, uint32_t resp_len);

#endif /* IONIC_ADMINQ_PRIV_H */
