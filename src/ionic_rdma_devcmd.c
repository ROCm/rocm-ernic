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
 * Copyright (C) 2025-2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <syslog.h>
#include <endian.h>
#include <sys/mman.h>

#include <vfio-user/libvfio-user.h>

#include "ionic_rdma_devcmd.h"
#include "ionic_eth_emu.h"
#include "ionic_adminq.h"
#include "ionic_datapath.h"

/* -------------------------------------------------------------------------
 * ionic_if.h constants (devcmd opcodes and structs)
 * Keep in sync with the pinned kernel ref.
 * -------------------------------------------------------------------------
 */
#define IONIC_CMD_RDMA_RESET_LIF     50
#define IONIC_CMD_RDMA_CREATE_EQ     51
#define IONIC_CMD_RDMA_CREATE_CQ     52
#define IONIC_CMD_RDMA_CREATE_ADMINQ 53

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

/* Max queues we track */
#define IONIC_MAX_EQ 32
#define IONIC_MAX_AQ 4

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
    struct ionic_rdma_aq aq[IONIC_MAX_AQ];
    int aq_count;

    /* Admin queue service context (ionic_adminq.c) */
    struct ionic_adminq_ctx *adminq_ctx;
};

static void rdma_cq_event_cb(void *opaque, uint32_t eq_id, uint32_t cq_id);

static int rdma_dma_write(vfu_ctx_t *vfu_ctx, uint64_t gpa, const void *buf,
                          size_t len)
{
    dma_sg_t *sg = malloc(dma_sg_size());
    struct iovec iov;
    int ret;

    if (!sg)
        return -ENOMEM;

    ret = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)(uintptr_t)gpa, len, sg, 1,
                          PROT_WRITE);
    if (ret < 0)
        goto out;

    ret = vfu_sgl_get(vfu_ctx, sg, &iov, 1, 0);
    if (ret < 0)
        goto out;

    memcpy(iov.iov_base, buf, len);
    vfu_sgl_mark_dirty(vfu_ctx, sg, 1);
    vfu_sgl_put(vfu_ctx, sg, &iov, 1);
    ret = 0;
out:
    free(sg);
    return ret;
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
    s->vfu_ctx = vfu_ctx;
    return s;
}

void ionic_rdma_devcmd_destroy(struct ionic_rdma_devcmd_state *s)
{
    if (!s)
        return;
    ionic_adminq_destroy(s->adminq_ctx);
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

    ionic_adminq_destroy(s->adminq_ctx);
    s->adminq_ctx = NULL;

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

    uint32_t qid_ver, cid;
    uint64_t dma_addr;
    memcpy(&qid_ver, cmd + RDMA_QUEUE_QID_VER_OFF, 4);
    memcpy(&cid, cmd + RDMA_QUEUE_CID_OFF, 4);
    memcpy(&dma_addr, cmd + RDMA_QUEUE_DMA_OFF, 8);
    qid_ver = le32toh(qid_ver);
    cid = le32toh(cid);
    dma_addr = le64toh(dma_addr);

    uint32_t qid = qid_ver & 0x00ffffffu;
    int idx = s->eq_count++;

    s->eq[idx].valid = true;
    s->eq[idx].qid = qid;
    s->eq[idx].intr_index = cid;
    s->eq[idx].dma_addr = dma_addr;
    s->eq[idx].depth_log2 = cmd[RDMA_QUEUE_DEPTH_OFF];
    s->eq[idx].stride_log2 = cmd[RDMA_QUEUE_STRIDE_OFF];
    s->eq[idx].prod = 0;
    s->eq[idx].color = 1;

    vfu_log(s->vfu_ctx, LOG_INFO,
            "ionic_rdma_devcmd: CREATE_EQ qid=%u intr=%u depth=2^%u "
            "dma=%#lx",
            qid, cid, s->eq[idx].depth_log2, dma_addr);

    comp[0] = 0; /* status OK */
}

static void handle_create_cq(struct ionic_rdma_devcmd_state *s,
                             const uint8_t *cmd, uint8_t *comp)
{
    uint32_t qid_ver, cid;
    uint64_t dma_addr;
    memcpy(&qid_ver, cmd + RDMA_QUEUE_QID_VER_OFF, 4);
    memcpy(&cid, cmd + RDMA_QUEUE_CID_OFF, 4);
    memcpy(&dma_addr, cmd + RDMA_QUEUE_DMA_OFF, 8);
    qid_ver = le32toh(qid_ver);
    cid = le32toh(cid);
    dma_addr = le64toh(dma_addr);

    uint32_t qid = qid_ver & 0x00ffffffu;

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
            "dma=%#lx",
            qid, s->pending_cq_depth_log2, dma_addr);

    comp[0] = 0;
}

static void handle_create_adminq(struct ionic_rdma_devcmd_state *s,
                                 const uint8_t *cmd, uint8_t *comp)
{
    if (s->aq_count >= IONIC_MAX_AQ) {
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

    uint32_t qid_ver;
    uint64_t dma_addr;
    memcpy(&qid_ver, cmd + RDMA_QUEUE_QID_VER_OFF, 4);
    memcpy(&dma_addr, cmd + RDMA_QUEUE_DMA_OFF, 8);
    qid_ver = le32toh(qid_ver);
    dma_addr = le64toh(dma_addr);

    uint32_t qid = qid_ver & 0x00ffffffu;
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
            "dma=%#lx cq_dma=%#lx",
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
    }
    ionic_adminq_set_cq_event_cb(s->adminq_ctx, rdma_cq_event_cb, s);
    ionic_adminq_register_queue(
        s->adminq_ctx, idx, dma_addr, s->aq[idx].depth_log2, s->pending_cq_dma,
        s->pending_cq_depth_log2, s->aq[idx].cq_id, s->aq[idx].eq_id);

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

    memset(comp, 0, 16);

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
    if (s)
        s->eth_emu = eth_emu;
}

void ionic_rdma_devcmd_set_datapath(struct ionic_rdma_devcmd_state *s,
                                    struct ionic_datapath *dp)
{
    /* Data-path CQEs need the same EQE-then-MSI-X sequence as admin CQEs. */
    if (s)
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
    uint32_t evt = (cq_id << 8) | (eq->color ? 1u : 0u);
    uint32_t be_evt = htobe32(evt);

    uint32_t depth = 1u << eq->depth_log2;
    uint64_t gpa = eq->dma_addr + (uint64_t)eq->prod * (1u << eq->stride_log2);

    vfu_log(s->vfu_ctx, LOG_DEBUG,
            "ionic_rdma_devcmd: EQE eq=%u cq=%u prod=%u color=%u gpa=%#lx",
            eq_id, cq_id, eq->prod, eq->color, (unsigned long)gpa);

    if (rdma_dma_write(s->vfu_ctx, gpa, &be_evt, sizeof(be_evt)) < 0) {
        vfu_log(s->vfu_ctx, LOG_ERR,
                "ionic_rdma_devcmd: EQE write to %#lx failed", gpa);
        return -EIO;
    }

    if (++eq->prod == depth) {
        eq->prod = 0;
        eq->color ^= 1;
    }

    if (!s->eth_emu)
        return 0;
    return ionic_eth_emu_trigger_irq(s->eth_emu, (int)eq->intr_index);
}

static void rdma_cq_event_cb(void *opaque, uint32_t eq_id, uint32_t cq_id)
{
    ionic_rdma_devcmd_post_cq_event(opaque, eq_id, cq_id);
}
