/*
 * ionic_datapath.c — ionic RDMA data-path emulation
 *
 * Handles the data plane for the ionic RDMA emulator:
 *
 *   1. Doorbell writes from the guest (BAR2) trigger WQE processing.
 *      The doorbell encodes: qtype (page offset / PAGE_SIZE), qid (24-bit),
 *      ring (0=normal, 1=arm CQ/EQ), and p_index (producer position).
 *
 *   2. WQE processing: read ionic_v1_wqe entries from the guest SQ/RQ ring,
 *      translate to the backend (rdma_rm / rdma_backend_loopback or verbs),
 *      and post ionic_v1_cqe completions to the paired CQ.
 *
 *   3. CQ notification: when a CQ is armed, fire the EQ interrupt so the
 *      driver knows to poll.
 *
 * This file implements the interface declared in ionic_datapath.h and is
 * called from:
 *   - ionic_eth_emu_bar2_access() on 8-byte doorbell writes
 *   - ionic_adminq.c when QP state changes to RTS (future)
 *
 * Wire formats: ionic_fw.h (ionic_v1_wqe, ionic_v1_cqe, ionic_v1_eqe).
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

#include "ionic_datapath.h"
#include "ionic_eth_emu.h"
#include "rocm_ernic_compat.h"

/* -------------------------------------------------------------------------
 * ionic_fw.h wire format constants (keep in sync with pinned kernel ref)
 * -------------------------------------------------------------------------
 */

/* WQE base header: 16 bytes (wqe_id[8], op[1], num_sge[1], flags[2], imm[4]) */

/* WQE opcodes (from ionic_fw.h enum ionic_v1_op; only used ones defined) */
#define IONIC_V1_OP_SEND      2
#define IONIC_V1_OP_SEND_IMM  3

/* WQE flags (big-endian be16 at byte 14; only SIG used currently) */
#define IONIC_V1_FLAG_SIG    0x0008u

/* ionic_v1_cqe layout (32 bytes, big-endian):
 *   union { recv { wqe_id[8], src_qpn_op[4], ... }; send { ... } } [0:23]
 *   be32 status_length  [24:27]
 *   be32 qid_type_flags [28:31]
 */
#define CQE_COLOR_BIT   0x01u
#define CQE_ERROR_BIT   0x02u
#define CQE_TYPE_RECV   (1u << 5)
#define CQE_TYPE_SEND   (2u << 5)

/* -------------------------------------------------------------------------
 * Per-QP ring state
 * -------------------------------------------------------------------------
 */

#define MAX_QP  (1u << 15)
#define MAX_CQ  (1u << 16)

struct ionic_qp_ring {
    bool     valid;
    uint64_t sq_dma;        /* guest PA of SQ WQE ring */
    uint64_t rq_dma;        /* guest PA of RQ WQE ring */
    uint32_t sq_depth;      /* entries = 2^depth_log2  */
    uint32_t rq_depth;
    uint8_t  sq_stride_log2;
    uint8_t  rq_stride_log2;
    uint32_t sq_cq_id;
    uint32_t rq_cq_id;
    uint32_t sq_prod;       /* guest producer index (last seen) */
    uint32_t rq_prod;
    uint32_t sq_cons;       /* our consumer index              */
    uint32_t rq_cons;
};

struct ionic_cq_ring {
    bool     valid;
    uint64_t dma;
    uint32_t depth;
    uint32_t prod;
    bool     color;    /* current expected color (flips on ring wrap) */
    bool     armed;    /* driver has armed this CQ for notification   */
    uint32_t eq_id;    /* EQ to fire when CQ has completions          */
};

struct ionic_datapath {
    vfu_ctx_t            *vfu_ctx;
    struct ionic_eth_emu *eth_emu;       /* for triggering EQ interrupts */
    pvrdma_handle_t       pvrdma_handle; /* for posting to RDMA backend  */

    struct ionic_qp_ring *qp;        /* indexed by qid   */
    struct ionic_cq_ring *cq;        /* indexed by cq_id */

    uint32_t qp_count;
    uint32_t cq_count;
};

/* -------------------------------------------------------------------------
 * Construction / destruction
 * -------------------------------------------------------------------------
 */

struct ionic_datapath *ionic_datapath_create(vfu_ctx_t *vfu_ctx,
                                              struct ionic_eth_emu *eth_emu)
{
    struct ionic_datapath *dp = calloc(1, sizeof(*dp));
    if (!dp)
        return NULL;

    dp->vfu_ctx  = vfu_ctx;
    dp->eth_emu  = eth_emu;

    dp->qp = calloc(MAX_QP, sizeof(*dp->qp));
    dp->cq = calloc(MAX_CQ, sizeof(*dp->cq));
    if (!dp->qp || !dp->cq) {
        free(dp->qp);
        free(dp->cq);
        free(dp);
        return NULL;
    }
    dp->qp_count = MAX_QP;
    dp->cq_count = MAX_CQ;
    return dp;
}

void ionic_datapath_set_pvrdma(struct ionic_datapath *dp, void *handle)
{
    if (dp)
        dp->pvrdma_handle = (pvrdma_handle_t)handle;
}

void ionic_datapath_destroy(struct ionic_datapath *dp)
{
    if (!dp)
        return;
    free(dp->qp);
    free(dp->cq);
    free(dp);
}

/* -------------------------------------------------------------------------
 * Register QP and CQ rings (called from ionic_adminq.c after CREATE_QP/CQ)
 * -------------------------------------------------------------------------
 */

void ionic_datapath_register_qp(struct ionic_datapath *dp, uint32_t qid,
                                 uint64_t sq_dma, uint8_t sq_depth_log2,
                                 uint8_t sq_stride_log2, uint32_t sq_cq_id,
                                 uint64_t rq_dma, uint8_t rq_depth_log2,
                                 uint8_t rq_stride_log2, uint32_t rq_cq_id)
{
    if (!dp || qid >= dp->qp_count)
        return;
    struct ionic_qp_ring *q = &dp->qp[qid];
    q->valid          = true;
    q->sq_dma         = sq_dma;
    q->rq_dma         = rq_dma;
    q->sq_depth       = 1u << sq_depth_log2;
    q->rq_depth       = 1u << rq_depth_log2;
    q->sq_stride_log2 = sq_stride_log2;
    q->rq_stride_log2 = rq_stride_log2;
    q->sq_cq_id       = sq_cq_id;
    q->rq_cq_id       = rq_cq_id;
    q->sq_prod = q->sq_cons = 0;
    q->rq_prod = q->rq_cons = 0;
}

void ionic_datapath_register_cq(struct ionic_datapath *dp, uint32_t cq_id,
                                 uint64_t dma, uint32_t depth, uint32_t eq_id)
{
    if (!dp || cq_id >= dp->cq_count)
        return;
    struct ionic_cq_ring *c = &dp->cq[cq_id];
    c->valid  = true;
    c->dma    = dma;
    c->depth  = depth;
    c->prod   = 0;
    c->color  = true;  /* initial color = true per ionic convention */
    c->armed  = false;
    c->eq_id  = eq_id;
}

/* -------------------------------------------------------------------------
 * DMA helpers (same pattern as ionic_adminq.c)
 * -------------------------------------------------------------------------
 */

static int dp_dma_read(vfu_ctx_t *vfu_ctx, uint64_t gpa, void *buf, size_t len)
{
    dma_sg_t *sg = malloc(dma_sg_size());
    struct iovec iov;
    int ret;

    if (!sg)
        return -ENOMEM;
    ret = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)(uintptr_t)gpa, len,
                          sg, 1, PROT_READ);
    if (ret < 0) { free(sg); return ret; }
    ret = vfu_sgl_get(vfu_ctx, sg, &iov, 1, 0);
    if (ret < 0) { free(sg); return ret; }
    memcpy(buf, iov.iov_base, len);
    vfu_sgl_put(vfu_ctx, sg, &iov, 1);
    free(sg);
    return 0;
}

static int dp_dma_write(vfu_ctx_t *vfu_ctx, uint64_t gpa,
                        const void *buf, size_t len)
{
    dma_sg_t *sg = malloc(dma_sg_size());
    struct iovec iov;
    int ret;

    if (!sg)
        return -ENOMEM;
    ret = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)(uintptr_t)gpa, len,
                          sg, 1, PROT_WRITE);
    if (ret < 0) { free(sg); return ret; }
    ret = vfu_sgl_get(vfu_ctx, sg, &iov, 1, 0);
    if (ret < 0) { free(sg); return ret; }
    memcpy(iov.iov_base, buf, len);
    vfu_sgl_mark_dirty(vfu_ctx, sg, 1);
    vfu_sgl_put(vfu_ctx, sg, &iov, 1);
    free(sg);
    return 0;
}

/* -------------------------------------------------------------------------
 * Post a CQE to a CQ ring and optionally fire the EQ
 * -------------------------------------------------------------------------
 */

static void post_data_cqe(struct ionic_datapath *dp,
                           uint32_t cq_id, uint32_t qid,
                           uint64_t wqe_id, uint8_t op, uint8_t status,
                           uint32_t byte_len, bool is_recv)
{
    if (cq_id >= dp->cq_count || !dp->cq[cq_id].valid)
        return;

    struct ionic_cq_ring *c = &dp->cq[cq_id];
    uint8_t cqe[32];
    memset(cqe, 0, 32);

    if (is_recv) {
        /* recv CQE: wqe_id[8] at byte 0, src_qpn_op[4] at 8 */
        uint64_t wid = htobe64(wqe_id);
        memcpy(cqe + 0, &wid, 8);
        uint32_t qpn_op = htobe32(((uint32_t)op << 24) | (qid & 0xffffffu));
        memcpy(cqe + 8, &qpn_op, 4);
    } else {
        /* send CQE: msg_msn at byte 4 */
        uint32_t msn = htobe32(c->prod);
        memcpy(cqe + 4, &msn, 4);
        uint64_t npg = htobe64(wqe_id);
        memcpy(cqe + 16, &npg, 8);
    }

    /* status_length at byte 24 (be32) */
    uint32_t sl = status ?
        htobe32((uint32_t)status << 24) : htobe32(byte_len);
    memcpy(cqe + 24, &sl, 4);

    /* qid_type_flags at byte 28 (be32):
     *   bit 0: color, bit 1: error, bits[7:5]: type, bits[31:8]: qid */
    uint32_t type = is_recv ? CQE_TYPE_RECV : CQE_TYPE_SEND;
    uint32_t qtf = (uint32_t)(c->color ? CQE_COLOR_BIT : 0) |
                   (status ? CQE_ERROR_BIT : 0) |
                   type |
                   ((qid & 0xffffffu) << 8);
    qtf = htobe32(qtf);
    memcpy(cqe + 28, &qtf, 4);

    uint64_t cqe_gpa = c->dma + (uint64_t)(c->prod % c->depth) * 32;
    if (dp_dma_write(dp->vfu_ctx, cqe_gpa, cqe, 32) < 0) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: CQE write failed for cq_id=%u", cq_id);
        return;
    }

    c->prod++;
    if (c->prod % c->depth == 0)
        c->color = !c->color;

    /* Fire EQ if CQ is armed */
    if (c->armed && dp->eth_emu) {
        c->armed = false;
        ionic_eth_emu_trigger_irq(dp->eth_emu, (int)c->eq_id);
    }
}

/* -------------------------------------------------------------------------
 * Loopback WQE processing
 *
 * For the loopback backend: matched SEND→RECV pairs.  For each SQ WQE,
 * find the matching RQ WQE on the same QP (loopback), copy data, post CQEs.
 *
 * Full verbs/TCP backend integration is deferred to a future commit.
 * -------------------------------------------------------------------------
 */

/*
 * ionic SGE layout in the WQE body (big-endian):
 *   be64 va; be32 len; be32 lkey   — 16 bytes each
 * The common body starts at byte 16 of the WQE (after the 16-byte header);
 * SGEs start at body+16 (after the 4-byte send/rdma subheader + 4-byte length).
 * Max SGEs from a 64-byte stride: (64 - 16 - 8) / 16 = 2.5 → 2 inline.
 * Larger strides allow more: (stride - 16 - 8) / 16.
 */
#define MAX_SGE_PER_WQE 16

/*
 * parse_sge: extract SGEs from a WQE buffer.
 *
 * @data:     pointer to start of WQE (byte 0 = wqe_id[0])
 * @num_sge:  SGE count from WQE header byte 9 (ionic_v1_base_hdr.num_sge_key)
 * @stride:   WQE stride in bytes (1 << stride_log2)
 *
 * ionic_v1_wqe layout (big-endian):
 *   [0:7]   wqe_id (be64)
 *   [8]     op
 *   [9]     num_sge_key  ← SGE count is here
 *   [10:11] flags (be16)
 *   [12:15] imm_data_key (be32)
 *   [16:23] subheader (send: ah_id+dest_qpn+dest_qkey; rdma: remote_va+rkey)
 *   [24:27] length (be32)
 *   [28...]  SGEs: {be64 va, be32 len, be32 lkey} × num_sge
 */
static int parse_sge(const uint8_t *data, uint8_t num_sge, uint32_t stride,
                     uint64_t *va_out, uint32_t *len_out, uint32_t *lkey_out)
{
    /* SGEs start at WQE byte 28 */
    const uint8_t *p = data + 28;
    uint32_t avail   = stride > 28 ? stride - 28 : 0;
    uint32_t max_sge = avail / 16;
    if (max_sge > MAX_SGE_PER_WQE)
        max_sge = MAX_SGE_PER_WQE;
    /* Clamp to num_sge from WQE header — this is the authoritative count.
     * Do not use null-termination: SGEs can legitimately have len=0 or lkey=0. */
    if ((uint32_t)num_sge < max_sge)
        max_sge = (uint32_t)num_sge;

    uint32_t n = 0;
    for (uint32_t i = 0; i < max_sge; i++) {
        uint64_t va;
        uint32_t len, lkey;
        memcpy(&va,   p + i * 16 + 0, 8);
        memcpy(&len,  p + i * 16 + 8, 4);
        memcpy(&lkey, p + i * 16 + 12, 4);
        va   = be64toh(va);
        len  = be32toh(len);
        lkey = be32toh(lkey);
        /* No null-termination sentinel — process all num_sge entries */
        va_out[n]   = va;
        len_out[n]  = len;
        lkey_out[n] = lkey;
        n++;
    }
    return (int)n;
}

static void process_sq_wqe(struct ionic_datapath *dp,
                             struct ionic_qp_ring *q, uint32_t qid,
                             uint32_t slot)
{
    uint32_t stride = 1u << q->sq_stride_log2;
    uint64_t wqe_gpa = q->sq_dma + (uint64_t)slot * stride;

    /* Read the full WQE (up to one stride = 64+ bytes). */
    uint8_t wqe[256];
    uint32_t read_sz = stride < sizeof(wqe) ? stride : sizeof(wqe);
    if (dp_dma_read(dp->vfu_ctx, wqe_gpa, wqe, read_sz) < 0)
        return;

    uint64_t wqe_id;
    memcpy(&wqe_id, wqe + 0, 8);
    wqe_id = be64toh(wqe_id);

    uint8_t  op      = wqe[8];
    uint8_t  num_sge_hdr = wqe[9];  /* num_sge from ionic_v1_base_hdr */
    uint16_t flags;
    memcpy(&flags, wqe + 10, 2);
    flags = be16toh(flags);

    /* Parse SGEs using the authoritative count from the WQE header byte 9. */
    uint64_t sge_va[MAX_SGE_PER_WQE];
    uint32_t sge_len[MAX_SGE_PER_WQE];
    uint32_t sge_lkey[MAX_SGE_PER_WQE];
    int num_sge = parse_sge(wqe, num_sge_hdr, stride, sge_va, sge_len, sge_lkey);
    if (num_sge < 0)
        num_sge = 0;

    /* Compute total payload length */
    uint32_t byte_len = 0;
    for (int i = 0; i < num_sge; i++)
        byte_len += sge_len[i];

    if (op == IONIC_V1_OP_SEND || op == IONIC_V1_OP_SEND_IMM) {
        /* Post via backend if available; fallback to loopback-only CQE. */
        if (dp->pvrdma_handle && num_sge > 0) {
            ionic_backend_post_send(dp->pvrdma_handle, qid,
                                    sge_va, sge_len, sge_lkey,
                                    (uint32_t)num_sge, op);
        }
        /* Post recv CQE on the peer RQ CQ (loopback: same QP). */
        post_data_cqe(dp, q->rq_cq_id, qid, wqe_id, op, 0, byte_len, true);
    }

    /* Post send completion when SIG flag set (or always for RC). */
    bool sig = (flags & IONIC_V1_FLAG_SIG) != 0;
    if (sig)
        post_data_cqe(dp, q->sq_cq_id, qid, wqe_id, op, 0, byte_len, false);
}

/* -------------------------------------------------------------------------
 * Doorbell handler
 *
 * Called by ionic_eth_emu_bar2_access() on 8-byte doorbell writes.
 *
 * Doorbell layout (struct ionic_doorbell, little-endian):
 *   le16 p_index   [0:1]  — new SQ/RQ/CQ producer index
 *   u8   ring      [2]    — 0=work, 1=arm CQ/EQ
 *   u8   qid_lo    [3]
 *   le16 qid_hi    [4:5]
 *   u16  rsvd      [6:7]
 * -------------------------------------------------------------------------
 */

void ionic_datapath_doorbell(struct ionic_datapath *dp,
                              int qtype, uint64_t doorbell_val)
{
    uint16_t p_index = (uint16_t)(doorbell_val & 0xffffu);
    uint8_t  ring    = (uint8_t)((doorbell_val >> 16) & 0xffu);
    uint32_t qid     = (uint32_t)(((doorbell_val >> 24) & 0xffu) |
                                   (((doorbell_val >> 32) & 0xffffu) << 8));

    vfu_log(dp->vfu_ctx, LOG_DEBUG,
            "ionic_datapath: doorbell qtype=%d qid=%u ring=%u p_index=%u",
            qtype, qid, ring, p_index);

    /* CQ arm (ring=1 or ring=2 for solicited-only) */
    if (ring == 1 || ring == 2) {
        if (qid < dp->cq_count && dp->cq[qid].valid)
            dp->cq[qid].armed = true;
        return;
    }

    /* SQ doorbell */
    if (qid >= dp->qp_count || !dp->qp[qid].valid)
        return;

    struct ionic_qp_ring *q = &dp->qp[qid];
    uint32_t new_prod = (uint32_t)p_index % q->sq_depth;

    while (q->sq_cons != new_prod) {
        process_sq_wqe(dp, q, qid, q->sq_cons % q->sq_depth);
        q->sq_cons = (q->sq_cons + 1) % q->sq_depth;
    }
    q->sq_prod = new_prod;
}
