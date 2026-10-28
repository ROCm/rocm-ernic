/*
 * ionic_rdma_devcmd.c — ionic RDMA devcmd handler (opcodes 50-53)
 *
 * ionic_rdma.ko issues four devcmds through the Ethernet admin queue
 * during its probe sequence:
 *
 *   50  IONIC_CMD_RDMA_RESET_LIF    — reset any previous RDMA state
 *   51  IONIC_CMD_RDMA_CREATE_EQ    — create event queue ring + MSI-X vector
 *   52  IONIC_CMD_RDMA_CREATE_CQ    — create admin CQ ring (per AQ)
 *   53  IONIC_CMD_RDMA_CREATE_ADMINQ — create RDMA admin queue ring
 *
 * The driver creates ≥4 EQs (IONIC_EQ_COUNT_MIN), 1 admin CQ per AQ, and
 * up to 4 admin queues (IONIC_AQ_COUNT).  After these complete, it registers
 * the IB device and userspace can use ibverbs.
 *
 * We DMA-map the guest ring buffers and store them in our queue table.
 * The admin queue is then serviced by ionic_adminq.c.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <syslog.h>

#include <vfio-user/libvfio-user.h>

#include "byteorder.h"
#include "ernic_dma.h"
#include "ernic_limits.h"
#include "ionic/ionic_bar.h"
#include "ionic/ionic_rdma_devcmd.h"
#include "ionic/ionic_eth_emu.h"
#include "ionic/ionic_adminq.h"
#include "ionic/ionic_datapath.h"

/* -------------------------------------------------------------------------
 * ionic_if.h structs (the opcodes are in ionic_rdma_devcmd.h)
 * Keep in sync with the pinned kernel ref.
 * -------------------------------------------------------------------------
 */

/*
 * struct ionic_rdma_reset_cmd (64 bytes):
 *   u8  opcode; u8 rsvd; le16 lif_index; u8 rsvd2[60]
 *
 * struct ionic_rdma_queue_cmd (64 bytes):
 *   u8  opcode; u8 rsvd; le16 lif_index;
 *   le32 qid_ver (qid | rdma_version<<24);
 *   le32 cid (EQ: intr index; CQ/AQ: eq_id/cq_id);
 *   le16 dbid; u8 depth_log2; u8 stride_log2; le64 dma_addr; u8 rsvd2[40]
 */
#define RDMA_QUEUE_QID_VER_OFF 4  /* le32 */
#define RDMA_QUEUE_CID_OFF     8  /* le32 */
#define RDMA_QUEUE_DEPTH_OFF   14 /* u8  */
#define RDMA_QUEUE_STRIDE_OFF  15 /* u8  */
#define RDMA_QUEUE_DMA_OFF     16 /* le64 */

/* Max queues we track; the admin queue count is the LIF identity's */
#define IONIC_MAX_EQ 32

/* An EQE (struct ionic_v1_eqe) is one be32, so an EQ's stride is at least
 * 2^2: anything smaller and each event would overwrite part of the next. */
#define EQ_MIN_STRIDE_LOG2 2u
_Static_assert((1u << EQ_MIN_STRIDE_LOG2) == sizeof(uint32_t),
               "EQ stride floor must be one EQE");

/* -------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------
 */

struct ionic_rdma_eq {
    bool valid;
    uint32_t qid;
    uint32_t intr_index; /* MSI-X vector assigned */
    uint64_t dma_addr;   /* guest PA of EQE ring  */
    uint8_t depth_log2;
    uint8_t stride_log2;
    uint32_t prod; /* next EQE slot to write */
    uint8_t color; /* colour bit the driver expects; ionic_create_eq()
                    * seeds eq->q.cons = true, so we start at 1 */
};

struct ionic_rdma_aq {
    bool valid;
    uint32_t qid;
    uint32_t cq_id;       /* paired admin CQ */
    uint32_t eq_id;       /* EQ that CQ raises events on */
    uint64_t dma_addr;    /* guest PA of WQE ring */
    uint64_t cq_dma_addr; /* guest PA of CQE ring */
    uint8_t depth_log2;
    uint8_t stride_log2;
    uint8_t cq_depth_log2;
    uint8_t cq_stride_log2;
};

struct ionic_rdma_devcmd_state {
    vfu_ctx_t *vfu_ctx;
    struct ernic_dma *dma; /* for the EQEs */

    /* Used to raise the MSI-X vector behind an EQ. */
    struct ionic_eth_emu *eth_emu;

    /* EQ table */
    struct ionic_rdma_eq eq[IONIC_MAX_EQ];
    int eq_count;

    /* Pending CQ (created with opcode 52, consumed by next opcode 53) */
    uint32_t pending_cq_qid;
    uint32_t pending_cq_eq_id;
    uint64_t pending_cq_dma;
    uint8_t pending_cq_depth_log2;
    uint8_t pending_cq_stride_log2;
    bool pending_cq_valid;

    /* AQ table */
    struct ionic_rdma_aq aq[ERNIC_MAX_AQ];
    int aq_count;

    /* Admin queue service context (ionic_adminq.c) */
    struct ionic_adminq_ctx *adminq_ctx;

    /* What every admin queue context is given when it's created, since one
     * is created at each CREATE_ADMINQ after an RDMA_RESET_LIF. */
    pvrdma_handle_t pvrdma_handle;
    struct ernic_stats *stats;
    struct ionic_fault_set *faults;
    struct ionic_datapath *dp;
};

static void rdma_cq_event_cb(void *opaque, uint32_t eq_id, uint32_t cq_id);

/* -------------------------------------------------------------------------
 * The admin queue context
 *
 * The Ethernet emulator keeps its own pointer to the context, to pass AQ
 * doorbells on, and this file is the only place that creates and frees it.
 * So this file keeps that pointer in step: a context freed while the
 * emulator still points at it lets the guest, with an AQ doorbell, read and
 * write freed memory.
 * -------------------------------------------------------------------------
 */

static void publish_adminq_ctx(const struct ionic_rdma_devcmd_state *s)
{
    if (s->eth_emu)
        ionic_eth_emu_register_adminq(s->eth_emu, s->adminq_ctx);
}

/* Give the context what this file was given; ionic_adminq ignores NULL. */
static void wire_adminq_ctx(struct ionic_rdma_devcmd_state *s)
{
    ionic_adminq_set_cq_event_cb(s->adminq_ctx, rdma_cq_event_cb, s);
    ionic_adminq_set_pvrdma(s->adminq_ctx, s->pvrdma_handle);
    ionic_adminq_set_stats(s->adminq_ctx, s->stats);
    ionic_adminq_set_faults(s->adminq_ctx, s->faults);
    ionic_adminq_set_datapath(s->adminq_ctx, s->dp);
}

static void drop_adminq_ctx(struct ionic_rdma_devcmd_state *s)
{
    struct ionic_adminq_ctx *ctx = s->adminq_ctx;

    s->adminq_ctx = NULL;
    publish_adminq_ctx(s);
    ionic_adminq_destroy(ctx);
}

/* -------------------------------------------------------------------------
 * Construction / destruction
 * -------------------------------------------------------------------------
 */

struct ionic_rdma_devcmd_state *ionic_rdma_devcmd_create(vfu_ctx_t *vfu_ctx)
{
    struct ionic_rdma_devcmd_state *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->dma = ernic_dma_create();
    if (!s->dma) {
        free(s);
        return NULL;
    }
    s->vfu_ctx = vfu_ctx;
    return s;
}

void ionic_rdma_devcmd_destroy(struct ionic_rdma_devcmd_state *s)
{
    if (!s)
        return;
    drop_adminq_ctx(s);
    ernic_dma_destroy(s->dma);
    free(s);
}

/* -------------------------------------------------------------------------
 * Command handlers
 * -------------------------------------------------------------------------
 */

static void handle_rdma_reset(struct ionic_rdma_devcmd_state *s,
                              const uint8_t *cmd, uint8_t *comp)
{
    (void)cmd;
    vfu_log(s->vfu_ctx, LOG_INFO, "ionic_rdma_devcmd: RDMA_RESET_LIF");

    /* Reset state */
    memset(s->eq, 0, sizeof(s->eq));
    s->eq_count = 0;
    memset(s->aq, 0, sizeof(s->aq));
    s->aq_count = 0;
    s->pending_cq_valid = false;

    /* Everything the driver made is gone with it, as on hardware. */
    ionic_adminq_release_all(s->adminq_ctx);
    drop_adminq_ctx(s);

    comp[0] = 0; /* status OK */
}

static void handle_create_eq(struct ionic_rdma_devcmd_state *s,
                             const uint8_t *cmd, uint8_t *comp)
{
    if (s->eq_count >= IONIC_MAX_EQ) {
        vfu_log(s->vfu_ctx, LOG_ERR,
                "ionic_rdma_devcmd: CREATE_EQ: too many EQs");
        comp[0] = 1; /* error */
        return;
    }

    uint32_t qid_ver = ernic_get_le32(cmd + RDMA_QUEUE_QID_VER_OFF);
    uint32_t cid = ernic_get_le32(cmd + RDMA_QUEUE_CID_OFF);
    uint64_t dma_addr = ernic_get_le64(cmd + RDMA_QUEUE_DMA_OFF);

    uint32_t qid = qid_ver & 0x00ffffffu;
    uint8_t depth_log2 = cmd[RDMA_QUEUE_DEPTH_OFF];
    uint8_t stride_log2 = cmd[RDMA_QUEUE_STRIDE_OFF];

    /* Guest exponents: each event's address is a shift by both. */
    if (!ionic_dp_ring_geometry_ok(depth_log2, stride_log2, EQ_MIN_STRIDE_LOG2,
                                   IONIC_RING_MAX_STRIDE_LOG2)) {
        vfu_log(s->vfu_ctx, LOG_ERR,
                "ionic_rdma_devcmd: CREATE_EQ qid=%u: refusing depth_log2=%u "
                "stride_log2=%u (depth max %u, stride %u..%u)",
                qid, depth_log2, stride_log2, IONIC_RING_MAX_DEPTH_LOG2,
                EQ_MIN_STRIDE_LOG2, IONIC_RING_MAX_STRIDE_LOG2);
        comp[0] = 1;
        return;
    }

    /* The interrupt the EQ raises. Past the MSI-X table it could never be
     * raised, and past INT_MAX it has no int to be passed as. */
    if (cid >= IONIC_MSIX_MAX_VECTORS) {
        vfu_log(s->vfu_ctx, LOG_ERR,
                "ionic_rdma_devcmd: CREATE_EQ qid=%u: refusing interrupt %u "
                "(max %u)",
                qid, cid, IONIC_MSIX_MAX_VECTORS - 1u);
        comp[0] = 1;
        return;
    }

    int idx = s->eq_count++;

    s->eq[idx].valid = true;
    s->eq[idx].qid = qid;
    s->eq[idx].intr_index = cid;
    s->eq[idx].dma_addr = dma_addr;
    s->eq[idx].depth_log2 = depth_log2;
    s->eq[idx].stride_log2 = stride_log2;
    s->eq[idx].prod = 0;
    s->eq[idx].color = 1;

    vfu_log(s->vfu_ctx, LOG_INFO,
            "ionic_rdma_devcmd: CREATE_EQ qid=%u intr=%u depth=2^%u "
            "dma=%#" PRIx64,
            qid, cid, s->eq[idx].depth_log2, dma_addr);

    comp[0] = 0; /* status OK */
}

static void handle_create_cq(struct ionic_rdma_devcmd_state *s,
                             const uint8_t *cmd, uint8_t *comp)
{
    uint32_t qid_ver = ernic_get_le32(cmd + RDMA_QUEUE_QID_VER_OFF);
    uint32_t cid = ernic_get_le32(cmd + RDMA_QUEUE_CID_OFF);
    uint64_t dma_addr = ernic_get_le64(cmd + RDMA_QUEUE_DMA_OFF);

    uint32_t qid = qid_ver & 0x00ffffffu;

    /* A guest exponent, which the admin queue sizes the CQ by shifting by.
     * Refused here, where the guest sent it, rather than at CREATE_ADMINQ.
     * The stride isn't used: admin CQEs have a fixed size. */
    if (cmd[RDMA_QUEUE_DEPTH_OFF] > IONIC_RING_MAX_DEPTH_LOG2) {
        vfu_log(s->vfu_ctx, LOG_ERR,
                "ionic_rdma_devcmd: CREATE_CQ (admin) qid=%u: refusing "
                "depth_log2=%u (max %u)",
                qid, cmd[RDMA_QUEUE_DEPTH_OFF], IONIC_RING_MAX_DEPTH_LOG2);
        comp[0] = 1;
        return;
    }

    /* Store as pending; the next CREATE_ADMINQ will consume it.  For a CQ,
     * cid is the id of the EQ that carries its notifications. */
    s->pending_cq_qid = qid;
    s->pending_cq_eq_id = cid;
    s->pending_cq_dma = dma_addr;
    s->pending_cq_depth_log2 = cmd[RDMA_QUEUE_DEPTH_OFF];
    s->pending_cq_stride_log2 = cmd[RDMA_QUEUE_STRIDE_OFF];
    s->pending_cq_valid = true;

    vfu_log(s->vfu_ctx, LOG_INFO,
            "ionic_rdma_devcmd: CREATE_CQ (admin) qid=%u depth=2^%u "
            "dma=%#" PRIx64,
            qid, s->pending_cq_depth_log2, dma_addr);

    comp[0] = 0;
}

static void handle_create_adminq(struct ionic_rdma_devcmd_state *s,
                                 const uint8_t *cmd, uint8_t *comp)
{
    if (s->aq_count >= ERNIC_MAX_AQ) {
        vfu_log(s->vfu_ctx, LOG_ERR,
                "ionic_rdma_devcmd: CREATE_ADMINQ: too many AQs");
        comp[0] = 1;
        return;
    }
    if (!s->pending_cq_valid) {
        vfu_log(s->vfu_ctx, LOG_ERR,
                "ionic_rdma_devcmd: CREATE_ADMINQ without prior CREATE_CQ");
        comp[0] = 1;
        return;
    }

    uint32_t qid_ver = ernic_get_le32(cmd + RDMA_QUEUE_QID_VER_OFF);
    uint64_t dma_addr = ernic_get_le64(cmd + RDMA_QUEUE_DMA_OFF);

    uint32_t qid = qid_ver & 0x00ffffffu;

    /* A guest exponent, which the admin queue sizes the AQ by shifting by.
     * The stride isn't used: admin WQEs have a fixed size. */
    if (cmd[RDMA_QUEUE_DEPTH_OFF] > IONIC_ADMINQ_MAX_DEPTH_LOG2) {
        vfu_log(s->vfu_ctx, LOG_ERR,
                "ionic_rdma_devcmd: CREATE_ADMINQ qid=%u: refusing "
                "depth_log2=%u (max %u)",
                qid, cmd[RDMA_QUEUE_DEPTH_OFF], IONIC_ADMINQ_MAX_DEPTH_LOG2);
        comp[0] = 1;
        return;
    }

    int idx = s->aq_count++;

    s->aq[idx].valid = true;
    s->aq[idx].qid = qid;
    s->aq[idx].cq_id = s->pending_cq_qid;
    s->aq[idx].eq_id = s->pending_cq_eq_id;
    s->aq[idx].dma_addr = dma_addr;
    s->aq[idx].cq_dma_addr = s->pending_cq_dma;
    s->aq[idx].depth_log2 = cmd[RDMA_QUEUE_DEPTH_OFF];
    s->aq[idx].stride_log2 = cmd[RDMA_QUEUE_STRIDE_OFF];
    s->aq[idx].cq_depth_log2 = s->pending_cq_depth_log2;
    s->aq[idx].cq_stride_log2 = s->pending_cq_stride_log2;
    s->pending_cq_valid = false;

    vfu_log(s->vfu_ctx, LOG_INFO,
            "ionic_rdma_devcmd: CREATE_ADMINQ qid=%u cq_id=%u depth=2^%u "
            "dma=%#" PRIx64 " cq_dma=%#" PRIx64,
            qid, s->aq[idx].cq_id, s->aq[idx].depth_log2, dma_addr,
            s->aq[idx].cq_dma_addr);

    /* Register this AQ with the admin queue service layer. */
    if (!s->adminq_ctx) {
        s->adminq_ctx = ionic_adminq_create(s->vfu_ctx);
        if (!s->adminq_ctx) {
            vfu_log(s->vfu_ctx, LOG_ERR,
                    "ionic_rdma_devcmd: failed to create adminq context");
            comp[0] = 1;
            return;
        }
        wire_adminq_ctx(s);
        publish_adminq_ctx(s);
    }
    if (!ionic_adminq_register_queue(s->adminq_ctx, idx, dma_addr,
                                     s->aq[idx].depth_log2, s->pending_cq_dma,
                                     s->pending_cq_depth_log2, s->aq[idx].cq_id,
                                     s->aq[idx].eq_id)) {
        memset(&s->aq[idx], 0, sizeof(s->aq[idx]));
        s->aq_count = idx;
        comp[0] = 1;
        return;
    }

    comp[0] = 0;
}

/* -------------------------------------------------------------------------
 * Main dispatch (called from ionic_eth_emu.c via the registered callback)
 * -------------------------------------------------------------------------
 */

void ionic_rdma_devcmd_dispatch(void *opaque, const uint8_t *cmd, uint8_t *comp)
{
    struct ionic_rdma_devcmd_state *s = opaque;
    uint8_t opcode = cmd[0];

    memset(comp, 0, IONIC_DEVCMD_COMP_LEN);

    switch (opcode) {
    case IONIC_CMD_RDMA_RESET_LIF:
        handle_rdma_reset(s, cmd, comp);
        break;
    case IONIC_CMD_RDMA_CREATE_EQ:
        handle_create_eq(s, cmd, comp);
        break;
    case IONIC_CMD_RDMA_CREATE_CQ:
        handle_create_cq(s, cmd, comp);
        break;
    case IONIC_CMD_RDMA_CREATE_ADMINQ:
        handle_create_adminq(s, cmd, comp);
        break;
    default:
        vfu_log(s->vfu_ctx, LOG_WARNING,
                "ionic_rdma_devcmd: unknown RDMA opcode=%u", opcode);
        comp[0] = 1;
        break;
    }
}

struct ionic_adminq_ctx *ionic_rdma_devcmd_get_adminq_ctx(
    struct ionic_rdma_devcmd_state *s)
{
    return s ? s->adminq_ctx : NULL;
}

void ionic_rdma_devcmd_set_eth_emu(struct ionic_rdma_devcmd_state *s,
                                   struct ionic_eth_emu *eth_emu)
{
    if (!s)
        return;
    s->eth_emu = eth_emu;
    publish_adminq_ctx(s);
}

void ionic_rdma_devcmd_set_pvrdma(struct ionic_rdma_devcmd_state *s,
                                  pvrdma_handle_t handle)
{
    if (!s)
        return;
    s->pvrdma_handle = handle;
    ionic_adminq_set_pvrdma(s->adminq_ctx, handle);
}

void ionic_rdma_devcmd_set_stats(struct ionic_rdma_devcmd_state *s,
                                 struct ernic_stats *stats)
{
    if (!s)
        return;
    s->stats = stats;
    ionic_adminq_set_stats(s->adminq_ctx, stats);
}

void ionic_rdma_devcmd_set_faults(struct ionic_rdma_devcmd_state *s,
                                  struct ionic_fault_set *faults)
{
    if (!s)
        return;
    s->faults = faults;
    ionic_adminq_set_faults(s->adminq_ctx, faults);
}

void ionic_rdma_devcmd_set_datapath(struct ionic_rdma_devcmd_state *s,
                                    struct ionic_datapath *dp)
{
    if (!s)
        return;
    s->dp = dp;
    ionic_adminq_set_datapath(s->adminq_ctx, dp);
    /* Data-path CQEs need the same EQE-then-MSI-X sequence as admin CQEs. */
    ionic_datapath_set_cq_event_cb(dp, rdma_cq_event_cb, s);
}

int ionic_rdma_devcmd_post_cq_event(struct ionic_rdma_devcmd_state *s,
                                    uint32_t eq_id, uint32_t cq_id)
{
    struct ionic_rdma_eq *eq = NULL;

    if (!s)
        return -EINVAL;

    for (int i = 0; i < s->eq_count; i++) {
        if (s->eq[i].valid && s->eq[i].qid == eq_id) {
            eq = &s->eq[i];
            break;
        }
    }
    if (!eq) {
        vfu_log(s->vfu_ctx, LOG_ERR,
                "ionic_rdma_devcmd: CQ event for unknown eq %u", eq_id);
        return -ENOENT;
    }

    /* struct ionic_v1_eqe is a single be32:
     *   bit 0     colour
     *   bits 3:1  type (0 = CQ)
     *   bits 7:4  code (0 = CQ_NOTIFY)
     *   bits 31:8 qid
     * The driver walks the ring until the colour stops matching, so the
     * entry has to be written before the interrupt is raised. */
    uint8_t eqe[4];
    ernic_put_be32(eqe, (cq_id << 8) | (eq->color ? 1u : 0u));

    /* Both exponents were bounded at CREATE_EQ, so the shifts are defined
     * and prod * stride < 2^(24 + 16) fits a uint64_t. */
    uint32_t depth = 1u << eq->depth_log2;
    uint64_t gpa = eq->dma_addr + (uint64_t)eq->prod * (1u << eq->stride_log2);

    vfu_log(s->vfu_ctx, LOG_DEBUG,
            "ionic_rdma_devcmd: EQE eq=%u cq=%u prod=%u color=%u gpa=%#" PRIx64,
            eq_id, cq_id, eq->prod, eq->color, gpa);

    if (ernic_dma_write(s->dma, s->vfu_ctx, gpa, eqe, sizeof(eqe)) < 0) {
        vfu_log(s->vfu_ctx, LOG_ERR,
                "ionic_rdma_devcmd: EQE write to %#" PRIx64 " failed", gpa);
        return -EIO;
    }

    if (++eq->prod == depth) {
        eq->prod = 0;
        eq->color ^= 1;
    }

    if (!s->eth_emu)
        return 0;
    /* CREATE_EQ held it below IONIC_MSIX_MAX_VECTORS, so it fits */
    return ionic_eth_emu_trigger_irq(s->eth_emu, (int)eq->intr_index);
}

static void rdma_cq_event_cb(void *opaque, uint32_t eq_id, uint32_t cq_id)
{
    ionic_rdma_devcmd_post_cq_event(opaque, eq_id, cq_id);
}
