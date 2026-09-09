/*
 * ionic_datapath.c — ionic RDMA data-path emulation
 *
 * Handles the data plane for the ionic RDMA emulator:
 *
 *   1. Doorbell writes from the guest (BAR2) trigger WQE processing.  The
 *      doorbell encodes qtype (BAR2 page offset), qid, ring (0 = work,
 *      1/2 = arm CQ) and p_index.
 *
 *   2. WQE processing: read ionic_v1_wqe entries from the guest SQ, match them
 *      against the destination QP's posted RQ WQEs, copy the payload directly
 *      between guest pages, and post ionic_v1_cqe completions to both CQs.
 *
 *   3. CQ notification: when a CQ is armed, raise an EQE on its EQ so the
 *      driver's interrupt handler polls it.
 *
 * SGEs name memory by (lkey, va, len) in the *client's* address space, so
 * every access goes through the MR table registered by CREATE_MR.  The
 * reserved lkey 0 (IONIC_DMA_LKEY) means the va is already a bus address.
 *
 * Wire formats: ionic_fw.h (ionic_v1_wqe, ionic_v1_cqe, ionic_sge).
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

/* enum ionic_v1_op */
#define IONIC_V1_OP_SEND           0
#define IONIC_V1_OP_SEND_INV       1
#define IONIC_V1_OP_SEND_IMM       2
#define IONIC_V1_OP_RDMA_READ      3
#define IONIC_V1_OP_RDMA_WRITE     4
#define IONIC_V1_OP_RDMA_WRITE_IMM 5
#define IONIC_V1_OP_ATOMIC_CS      6
#define IONIC_V1_OP_ATOMIC_FA      7

/* enum ionic_v1_flag (be16 at WQE byte 10) */
#define IONIC_V1_FLAG_INL 0x0004u
#define IONIC_V1_FLAG_SIG 0x0008u

/*
 * struct ionic_v1_wqe:
 *   [0:7]   u64  wqe_id (native endian: SQ producer index / RQ meta index)
 *   [8]     u8   op
 *   [9]     u8   num_sge_key
 *   [10:11] be16 flags
 *   [12:15] be32 imm_data_key  (recv: total posted length)
 *   send common body:
 *     [16:19] be32 ah_id
 *     [20:23] be32 dest_qpn
 *     [24:27] be32 dest_qkey
 *     [28:31] be32 length
 *   recv body: [16:31] reserved
 *   [32...] payload: struct ionic_sge { be64 va; be32 len; be32 lkey; }
 */
#define WQE_PLD_OFF 32
#define WQE_SEND_LEN_OFF 28

/* struct ionic_v1_common_bdy.rdma overlays the send body: two be32 halves of
 * the remote va at 16/20, then the remote rkey, then the shared length. */
#define WQE_RDMA_VA_HI_OFF 16
#define WQE_RDMA_VA_LO_OFF 20
#define WQE_RDMA_RKEY_OFF  24

/* struct ionic_v1_atomic_bdy shares the first three fields with the rdma body
 * and then carries its operands and a single fixed 8-byte result SGE. */
#define WQE_ATOMIC_SWAP_ADD_OFF 28
#define WQE_ATOMIC_COMPARE_OFF  36
#define WQE_ATOMIC_SGE_OFF      48

/* struct ionic_v1_cqe (32 bytes) */
#define CQE_SIZE 32
#define CQE_COLOR_BIT 0x01u
#define CQE_ERROR_BIT 0x02u
#define CQE_TYPE_RECV     (1u << 5)
#define CQE_TYPE_SEND_MSN (2u << 5)
#define CQE_TYPE_SEND_NPG (3u << 5)

/* enum ionic_v1_cqe_src_qpn_bits */
#define CQE_RECV_OP_SHIFT 24
#define CQE_RECV_OP_SEND     0
#define CQE_RECV_OP_SEND_INV 1
#define CQE_RECV_OP_SEND_IMM 2
#define CQE_RECV_OP_RDMA_IMM 3

/* IONIC_STS_LOCAL_LEN_ERR — recv buffer too small for the inbound message. */
#define IONIC_STS_OK             0
#define IONIC_STS_LOCAL_LEN_ERR  1
#define IONIC_STS_REMOTE_ACC_ERR 9

/* -------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------
 */

#define MAX_QP  (1u << 15)
#define MAX_CQ  (1u << 16)
#define MAX_MR  1024
#define MAX_SGE 32

/* A guest buffer: either directly addressed or described by a page table. */
struct dp_buf {
    uint64_t base;    /* direct GPA when npages <= 1                    */
    uint64_t *pages;  /* page-table contents when npages > 1            */
    uint32_t npages;
    uint8_t page_size_log2;
    uint32_t first_off; /* byte offset of the region within page 0      */
};

struct ionic_qp_ring {
    bool valid;
    uint8_t ib_qp_type;

    struct dp_buf sq_buf;
    struct dp_buf rq_buf;
    uint32_t sq_depth;
    uint32_t rq_depth;
    uint8_t sq_stride_log2;
    uint8_t rq_stride_log2;
    uint32_t sq_cq_id;
    uint32_t rq_cq_id;

    uint32_t sq_cons;
    uint32_t rq_prod;
    uint32_t rq_cons;
    uint32_t msn; /* running send sequence for SEND_MSN completions */

    bool dest_valid;
    uint32_t dest_qp_id;
};

struct ionic_cq_ring {
    bool valid;
    struct dp_buf buf;
    uint32_t depth;
    uint8_t stride_log2;
    uint32_t prod;
    bool color;
    bool armed;
    uint32_t eq_id;
};

struct dp_mr {
    bool valid;
    uint32_t lkey;
    uint64_t va;
    uint64_t length;
    struct dp_buf buf;
};

struct ionic_datapath {
    vfu_ctx_t *vfu_ctx;
    struct ionic_eth_emu *eth_emu;
    pvrdma_handle_t pvrdma_handle;

    ionic_dp_cq_event_fn_t cq_event_fn;
    void *cq_event_opaque;

    struct ionic_qp_ring *qp; /* indexed by driver qp_id */
    struct ionic_cq_ring *cq; /* indexed by driver cq_id */
    struct dp_mr mr[MAX_MR];

    uint32_t qp_count;
    uint32_t cq_count;
};

/* -------------------------------------------------------------------------
 * DMA helpers
 * -------------------------------------------------------------------------
 */

static int dp_dma_rw(vfu_ctx_t *vfu_ctx, uint64_t gpa, void *buf, size_t len,
                     bool write)
{
    dma_sg_t *sg = malloc(dma_sg_size());
    struct iovec iov;
    int ret;

    if (!sg)
        return -ENOMEM;
    ret = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)(uintptr_t)gpa, len, sg, 1,
                          write ? PROT_WRITE : PROT_READ);
    if (ret < 0) {
        free(sg);
        return ret;
    }
    ret = vfu_sgl_get(vfu_ctx, sg, &iov, 1, 0);
    if (ret < 0) {
        free(sg);
        return ret;
    }
    if (write) {
        memcpy(iov.iov_base, buf, len);
        vfu_sgl_mark_dirty(vfu_ctx, sg, 1);
    } else {
        memcpy(buf, iov.iov_base, len);
    }
    vfu_sgl_put(vfu_ctx, sg, &iov, 1);
    free(sg);
    return 0;
}

static int dp_dma_read(vfu_ctx_t *vfu_ctx, uint64_t gpa, void *buf, size_t len)
{
    return dp_dma_rw(vfu_ctx, gpa, buf, len, false);
}

static int dp_dma_write(vfu_ctx_t *vfu_ctx, uint64_t gpa, const void *buf,
                        size_t len)
{
    /* dp_dma_rw does not modify buf on the write path. */
    return dp_dma_rw(vfu_ctx, gpa, (void *)(uintptr_t)buf, len, true);
}

/* -------------------------------------------------------------------------
 * Page-table backed buffers
 * -------------------------------------------------------------------------
 */

static void buf_release(struct dp_buf *b)
{
    free(b->pages);
    memset(b, 0, sizeof(*b));
}

static int buf_init(struct ionic_datapath *dp, struct dp_buf *b,
                    const struct ionic_dp_buf_desc *desc, uint64_t va)
{
    buf_release(b);
    b->page_size_log2 = desc->page_size_log2 ? desc->page_size_log2 : 12;
    b->npages = desc->map_count;

    if (desc->map_count <= 1) {
        /* ionic_pgtbl_dma() already folded the intra-page offset in. */
        b->base = desc->dma_addr;
        b->npages = 1;
        return 0;
    }

    b->pages = calloc(desc->map_count, sizeof(*b->pages));
    if (!b->pages) {
        b->npages = 0;
        return -ENOMEM;
    }
    if (dp_dma_read(dp->vfu_ctx, desc->dma_addr, b->pages,
                    (size_t)desc->map_count * 8) < 0) {
        buf_release(b);
        return -EIO;
    }
    for (uint32_t i = 0; i < desc->map_count; i++)
        b->pages[i] = le64toh(b->pages[i]);

    b->first_off = (uint32_t)(va & ((1ull << b->page_size_log2) - 1));
    return 0;
}

/*
 * Resolve a byte offset within a buffer to a guest physical address, and
 * report how many bytes remain contiguous from there.
 */
static uint64_t buf_gpa(const struct dp_buf *b, uint64_t off, uint64_t *run)
{
    if (b->npages <= 1 || !b->pages) {
        *run = UINT64_MAX;
        return b->base + off;
    }

    uint64_t abs = off + b->first_off;
    uint64_t psz = 1ull << b->page_size_log2;
    uint64_t idx = abs >> b->page_size_log2;
    uint64_t poff = abs & (psz - 1);

    if (idx >= b->npages) {
        *run = 0;
        return 0;
    }
    *run = psz - poff;
    return b->pages[idx] + poff;
}

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

    dp->vfu_ctx = vfu_ctx;
    dp->eth_emu = eth_emu;

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

void ionic_datapath_set_cq_event_cb(struct ionic_datapath *dp,
                                    ionic_dp_cq_event_fn_t fn, void *opaque)
{
    if (!dp)
        return;
    dp->cq_event_fn = fn;
    dp->cq_event_opaque = opaque;
}

void ionic_datapath_destroy(struct ionic_datapath *dp)
{
    if (!dp)
        return;
    for (uint32_t i = 0; i < dp->qp_count; i++) {
        buf_release(&dp->qp[i].sq_buf);
        buf_release(&dp->qp[i].rq_buf);
    }
    for (uint32_t i = 0; i < dp->cq_count; i++)
        buf_release(&dp->cq[i].buf);
    for (int i = 0; i < MAX_MR; i++)
        buf_release(&dp->mr[i].buf);
    free(dp->qp);
    free(dp->cq);
    free(dp);
}

/* -------------------------------------------------------------------------
 * Registration
 * -------------------------------------------------------------------------
 */

void ionic_datapath_register_cq(struct ionic_datapath *dp, uint32_t cq_id,
                                uint32_t eq_id,
                                const struct ionic_dp_ring_desc *ring)
{
    if (!dp || cq_id >= dp->cq_count)
        return;

    struct ionic_cq_ring *c = &dp->cq[cq_id];
    if (buf_init(dp, &c->buf, &ring->buf, 0) < 0) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: CQ %u page table read failed", cq_id);
        return;
    }
    c->depth = 1u << ring->depth_log2;
    c->stride_log2 = ring->stride_log2 ? ring->stride_log2 : 5;
    c->prod = 0;
    c->color = true; /* the driver's cq->color also starts true */
    c->armed = false;
    c->eq_id = eq_id;
    c->valid = true;

    vfu_log(dp->vfu_ctx, LOG_INFO,
            "ionic_datapath: CQ %u depth=%u stride=2^%u eq=%u pages=%u",
            cq_id, c->depth, c->stride_log2, eq_id, c->buf.npages);
}

void ionic_datapath_unregister_cq(struct ionic_datapath *dp, uint32_t cq_id)
{
    if (!dp || cq_id >= dp->cq_count)
        return;
    buf_release(&dp->cq[cq_id].buf);
    dp->cq[cq_id].valid = false;
}

void ionic_datapath_register_qp(struct ionic_datapath *dp, uint32_t qp_id,
                                uint8_t ib_qp_type, uint32_t sq_cq_id,
                                const struct ionic_dp_ring_desc *sq,
                                uint32_t rq_cq_id,
                                const struct ionic_dp_ring_desc *rq)
{
    if (!dp || qp_id >= dp->qp_count)
        return;

    struct ionic_qp_ring *q = &dp->qp[qp_id];
    if (buf_init(dp, &q->sq_buf, &sq->buf, 0) < 0 ||
        buf_init(dp, &q->rq_buf, &rq->buf, 0) < 0) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: QP %u page table read failed", qp_id);
        return;
    }
    q->ib_qp_type = ib_qp_type;
    q->sq_depth = 1u << sq->depth_log2;
    q->rq_depth = 1u << rq->depth_log2;
    q->sq_stride_log2 = sq->stride_log2;
    q->rq_stride_log2 = rq->stride_log2;
    q->sq_cq_id = sq_cq_id;
    q->rq_cq_id = rq_cq_id;
    q->sq_cons = 0;
    q->rq_prod = q->rq_cons = 0;
    q->msn = 0;
    q->dest_valid = false;
    q->valid = true;

    vfu_log(dp->vfu_ctx, LOG_INFO,
            "ionic_datapath: QP %u type=%u sq(depth=%u stride=2^%u cq=%u) "
            "rq(depth=%u stride=2^%u cq=%u)",
            qp_id, ib_qp_type, q->sq_depth, q->sq_stride_log2, sq_cq_id,
            q->rq_depth, q->rq_stride_log2, rq_cq_id);
}

void ionic_datapath_unregister_qp(struct ionic_datapath *dp, uint32_t qp_id)
{
    if (!dp || qp_id >= dp->qp_count)
        return;
    buf_release(&dp->qp[qp_id].sq_buf);
    buf_release(&dp->qp[qp_id].rq_buf);
    dp->qp[qp_id].valid = false;
}

void ionic_datapath_set_dest_qp(struct ionic_datapath *dp, uint32_t qp_id,
                                uint32_t dest_qp_id)
{
    if (!dp || qp_id >= dp->qp_count || !dp->qp[qp_id].valid)
        return;
    dp->qp[qp_id].dest_qp_id = dest_qp_id;
    dp->qp[qp_id].dest_valid = true;
}

static struct dp_mr *mr_find(struct ionic_datapath *dp, uint32_t lkey)
{
    for (int i = 0; i < MAX_MR; i++)
        if (dp->mr[i].valid && dp->mr[i].lkey == lkey)
            return &dp->mr[i];
    return NULL;
}

void ionic_datapath_register_mr(struct ionic_datapath *dp, uint32_t lkey,
                                uint64_t va, uint64_t length,
                                const struct ionic_dp_buf_desc *buf)
{
    if (!dp)
        return;

    struct dp_mr *m = mr_find(dp, lkey);
    if (!m) {
        for (int i = 0; i < MAX_MR; i++) {
            if (!dp->mr[i].valid) {
                m = &dp->mr[i];
                break;
            }
        }
    }
    if (!m) {
        vfu_log(dp->vfu_ctx, LOG_WARNING, "ionic_datapath: MR table full");
        return;
    }

    if (buf_init(dp, &m->buf, buf, va) < 0) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: MR %#x page table read failed", lkey);
        return;
    }
    m->lkey = lkey;
    m->va = va;
    m->length = length;
    m->valid = true;

    vfu_log(dp->vfu_ctx, LOG_INFO,
            "ionic_datapath: MR lkey=%#x va=%#lx len=%lu pages=%u pgsz=2^%u",
            lkey, (unsigned long)va, (unsigned long)length, m->buf.npages,
            m->buf.page_size_log2);
}

void ionic_datapath_unregister_mr(struct ionic_datapath *dp, uint32_t lkey)
{
    struct dp_mr *m = dp ? mr_find(dp, lkey) : NULL;
    if (!m)
        return;
    buf_release(&m->buf);
    m->valid = false;
}

/* -------------------------------------------------------------------------
 * SGE address translation
 * -------------------------------------------------------------------------
 */

/*
 * Resolve one (lkey, va) pair to a guest physical address, reporting how many
 * bytes stay contiguous.  lkey 0 is IONIC_DMA_LKEY: the va is already a bus
 * address, which is what the kernel uses for its own DMA-mapped buffers.
 */
static uint64_t sge_gpa(struct ionic_datapath *dp, uint32_t lkey, uint64_t va,
                        uint64_t *run)
{
    if (lkey == 0) {
        *run = UINT64_MAX;
        return va;
    }

    struct dp_mr *m = mr_find(dp, lkey);
    if (!m || va < m->va || va >= m->va + m->length) {
        *run = 0;
        return 0;
    }
    return buf_gpa(&m->buf, va - m->va, run);
}

/*
 * Copy @len bytes between two SGE-described regions, walking both sides page
 * by page.  Returns the number of bytes copied.
 */
static uint32_t dp_copy_sge(struct ionic_datapath *dp, uint32_t dst_lkey,
                            uint64_t dst_va, uint32_t src_lkey,
                            uint64_t src_va, uint32_t len)
{
    uint8_t bounce[4096];
    uint32_t done = 0;

    while (done < len) {
        uint64_t src_run, dst_run;
        uint64_t src = sge_gpa(dp, src_lkey, src_va + done, &src_run);
        uint64_t dst = sge_gpa(dp, dst_lkey, dst_va + done, &dst_run);
        if (!src_run || !dst_run)
            break;

        uint64_t chunk = len - done;
        if (chunk > src_run)
            chunk = src_run;
        if (chunk > dst_run)
            chunk = dst_run;
        if (chunk > sizeof(bounce))
            chunk = sizeof(bounce);

        if (dp_dma_read(dp->vfu_ctx, src, bounce, (size_t)chunk) < 0)
            break;
        if (dp_dma_write(dp->vfu_ctx, dst, bounce, (size_t)chunk) < 0)
            break;
        done += (uint32_t)chunk;
    }
    return done;
}

/* -------------------------------------------------------------------------
 * CQ posting
 * -------------------------------------------------------------------------
 */

static void cq_post(struct ionic_datapath *dp, uint32_t cq_id,
                    const uint8_t cqe_body[CQE_SIZE - 8], uint32_t status_len,
                    uint32_t type, uint32_t qid, bool error)
{
    if (cq_id >= dp->cq_count || !dp->cq[cq_id].valid)
        return;

    struct ionic_cq_ring *c = &dp->cq[cq_id];
    uint8_t cqe[CQE_SIZE];

    memcpy(cqe, cqe_body, CQE_SIZE - 8);

    uint32_t sl = htobe32(status_len);
    memcpy(cqe + 24, &sl, 4);

    uint32_t qtf = (uint32_t)(c->color ? CQE_COLOR_BIT : 0) |
                   (error ? CQE_ERROR_BIT : 0) | type |
                   ((qid & 0xffffffu) << 8);
    qtf = htobe32(qtf);
    memcpy(cqe + 28, &qtf, 4);

    uint32_t stride = 1u << c->stride_log2;
    uint64_t run;
    uint64_t gpa = buf_gpa(&c->buf, (uint64_t)(c->prod % c->depth) * stride,
                           &run);
    if (!run || dp_dma_write(dp->vfu_ctx, gpa, cqe, CQE_SIZE) < 0) {
        vfu_log(dp->vfu_ctx, LOG_ERR,
                "ionic_datapath: CQE write failed for cq_id=%u", cq_id);
        return;
    }

    c->prod++;
    if (c->prod % c->depth == 0)
        c->color = !c->color;

    if (c->armed) {
        c->armed = false;
        if (dp->cq_event_fn)
            dp->cq_event_fn(dp->cq_event_opaque, c->eq_id, cq_id);
        else if (dp->eth_emu)
            ionic_eth_emu_trigger_irq(dp->eth_emu, (int)c->eq_id);
    }
}

static void cq_post_recv(struct ionic_datapath *dp, uint32_t cq_id,
                         uint32_t qid, uint64_t rq_wqe_id, uint32_t src_qpn,
                         uint8_t recv_op, uint32_t imm_be, uint32_t byte_len,
                         bool error)
{
    uint8_t body[CQE_SIZE - 8];
    memset(body, 0, sizeof(body));

    /* recv.wqe_id is a native u64 the driver indexes rq_meta with. */
    memcpy(body + 0, &rq_wqe_id, 8);

    uint32_t qpn_op =
        htobe32(((uint32_t)recv_op << CQE_RECV_OP_SHIFT) | (src_qpn & 0xffffffu));
    memcpy(body + 8, &qpn_op, 4);
    memcpy(body + 20, &imm_be, 4); /* recv.imm_data_rkey, already be32 */

    cq_post(dp, cq_id, body, byte_len, CQE_TYPE_RECV, qid, error);
}

static void cq_post_send_msn(struct ionic_datapath *dp, uint32_t cq_id,
                             uint32_t qid, uint32_t msn, uint32_t status)
{
    uint8_t body[CQE_SIZE - 8];
    memset(body, 0, sizeof(body));

    uint32_t m = htobe32(msn);
    memcpy(body + 4, &m, 4); /* send.msg_msn */

    cq_post(dp, cq_id, body, status, CQE_TYPE_SEND_MSN, qid,
            status != IONIC_STS_OK);
}

static void cq_post_send_npg(struct ionic_datapath *dp, uint32_t cq_id,
                             uint32_t qid, uint64_t sq_wqe_id)
{
    uint8_t body[CQE_SIZE - 8];
    memset(body, 0, sizeof(body));

    /* send.npg_wqe_id is a native u64 masked with sq.mask by the driver. */
    memcpy(body + 16, &sq_wqe_id, 8);

    cq_post(dp, cq_id, body, 0, CQE_TYPE_SEND_NPG, qid, false);
}

/* -------------------------------------------------------------------------
 * WQE processing
 * -------------------------------------------------------------------------
 */

struct dp_sge_list {
    uint64_t va[MAX_SGE];
    uint32_t len[MAX_SGE];
    uint32_t lkey[MAX_SGE];
    uint32_t count;
    uint32_t total;
};

static void parse_sges(const uint8_t *wqe, uint32_t stride, uint8_t num_sge,
                       struct dp_sge_list *out)
{
    uint32_t avail = stride > WQE_PLD_OFF ? stride - WQE_PLD_OFF : 0;
    uint32_t max_sge = avail / 16;

    out->count = 0;
    out->total = 0;
    if (max_sge > MAX_SGE)
        max_sge = MAX_SGE;
    if (num_sge < max_sge)
        max_sge = num_sge;

    for (uint32_t i = 0; i < max_sge; i++) {
        const uint8_t *p = wqe + WQE_PLD_OFF + i * 16;
        uint64_t va;
        uint32_t len, lkey;
        memcpy(&va, p + 0, 8);
        memcpy(&len, p + 8, 4);
        memcpy(&lkey, p + 12, 4);
        out->va[i] = be64toh(va);
        out->len[i] = be32toh(len);
        out->lkey[i] = be32toh(lkey);
        out->total += out->len[i];
        out->count++;
    }
}

/*
 * Deliver a SEND payload into the destination QP's next posted receive.
 * Returns the number of bytes delivered, or -1 if no receive was available.
 */
static int64_t deliver_recv(struct ionic_datapath *dp,
                            struct ionic_qp_ring *dq, uint32_t dst_qp_id,
                            uint32_t src_qp_id, const struct dp_sge_list *src,
                            uint8_t recv_op, uint32_t imm_be)
{
    if (dq->rq_cons == dq->rq_prod)
        return -1;

    uint32_t stride = 1u << dq->rq_stride_log2;
    uint32_t slot = dq->rq_cons % dq->rq_depth;
    uint64_t run;
    uint64_t gpa = buf_gpa(&dq->rq_buf, (uint64_t)slot * stride, &run);

    uint8_t rwqe[256];
    uint32_t read_sz = stride < sizeof(rwqe) ? stride : (uint32_t)sizeof(rwqe);
    if (!run || run < read_sz ||
        dp_dma_read(dp->vfu_ctx, gpa, rwqe, read_sz) < 0)
        return -1;

    uint64_t rq_wqe_id;
    memcpy(&rq_wqe_id, rwqe + 0, 8);

    struct dp_sge_list dst;
    parse_sges(rwqe, stride, rwqe[9], &dst);

    dq->rq_cons++;

    /* Walk both SGE lists in lockstep, copying the overlap. */
    uint32_t copied = 0;
    uint32_t si = 0, di = 0, soff = 0, doff = 0;
    while (si < src->count && di < dst.count) {
        uint32_t s_rem = src->len[si] - soff;
        uint32_t d_rem = dst.len[di] - doff;
        uint32_t chunk = s_rem < d_rem ? s_rem : d_rem;

        if (chunk) {
            uint32_t n = dp_copy_sge(dp, dst.lkey[di], dst.va[di] + doff,
                                     src->lkey[si], src->va[si] + soff, chunk);
            copied += n;
            if (n != chunk)
                break;
        }
        soff += chunk;
        doff += chunk;
        if (soff == src->len[si]) {
            si++;
            soff = 0;
        }
        if (doff == dst.len[di]) {
            di++;
            doff = 0;
        }
    }

    bool truncated = copied < src->total;
    cq_post_recv(dp, dq->rq_cq_id, dst_qp_id, rq_wqe_id, src_qp_id, recv_op,
                 imm_be, truncated ? IONIC_STS_LOCAL_LEN_ERR : copied,
                 truncated);

    return copied;
}

/*
 * Move @len bytes between a local SGE list and a remote region named by
 * (rkey, remote_va).  The remote side is linear, so it is just an offset walk;
 * the local side is scattered.  Returns the number of bytes transferred.
 *
 * Both ends resolve through the same MR table: an rkey is an lkey here,
 * because the emulator holds every registration for the one guest it serves.
 */
static uint32_t rdma_xfer(struct ionic_datapath *dp,
                          const struct dp_sge_list *local, uint32_t rkey,
                          uint64_t remote_va, uint32_t len, bool to_remote)
{
    uint32_t done = 0;

    for (uint32_t i = 0; i < local->count && done < len; i++) {
        uint32_t chunk = local->len[i];
        if (chunk > len - done)
            chunk = len - done;
        if (!chunk)
            continue;

        uint32_t n = to_remote
                         ? dp_copy_sge(dp, rkey, remote_va + done,
                                       local->lkey[i], local->va[i], chunk)
                         : dp_copy_sge(dp, local->lkey[i], local->va[i], rkey,
                                       remote_va + done, chunk);
        done += n;
        if (n != chunk)
            break;
    }
    return done;
}

/* Read the be32 pair at @off as one 64-bit value: the wire splits every
 * address and operand into high and low halves. */
static uint64_t wqe_be64_pair(const uint8_t *wqe, uint32_t off)
{
    uint32_t hi, lo;
    memcpy(&hi, wqe + off, 4);
    memcpy(&lo, wqe + off + 4, 4);
    return ((uint64_t)be32toh(hi) << 32) | be32toh(lo);
}

/*
 * Compare-and-swap or fetch-and-add on 8 bytes of remote memory, with the
 * original value returned to the local SGE.  Both operands and remote memory
 * are treated as host-order u64: the driver converts the caller's values to
 * big endian for the wire, so undoing that yields exactly what the
 * application passed, and the same guest owns both sides of the copy.
 */
static bool do_atomic(struct ionic_datapath *dp, const uint8_t *wqe,
                      bool compare_swap)
{
    uint64_t remote_va = wqe_be64_pair(wqe, WQE_RDMA_VA_HI_OFF);
    uint64_t swap_add = wqe_be64_pair(wqe, WQE_ATOMIC_SWAP_ADD_OFF);
    uint64_t compare = wqe_be64_pair(wqe, WQE_ATOMIC_COMPARE_OFF);
    uint32_t rkey;
    memcpy(&rkey, wqe + WQE_RDMA_RKEY_OFF, 4);
    rkey = be32toh(rkey);

    uint64_t local_va, run;
    uint32_t local_lkey;
    memcpy(&local_va, wqe + WQE_ATOMIC_SGE_OFF, 8);
    memcpy(&local_lkey, wqe + WQE_ATOMIC_SGE_OFF + 12, 4);
    local_va = be64toh(local_va);
    local_lkey = be32toh(local_lkey);

    uint64_t rgpa = sge_gpa(dp, rkey, remote_va, &run);
    if (run < 8)
        return false;
    uint64_t lgpa = sge_gpa(dp, local_lkey, local_va, &run);
    if (run < 8)
        return false;

    uint64_t old;
    if (dp_dma_read(dp->vfu_ctx, rgpa, &old, 8) < 0)
        return false;

    uint64_t new = compare_swap ? (old == compare ? swap_add : old)
                                : old + swap_add;

    if (dp_dma_write(dp->vfu_ctx, rgpa, &new, 8) < 0)
        return false;
    return dp_dma_write(dp->vfu_ctx, lgpa, &old, 8) == 0;
}

static void process_sq_wqe(struct ionic_datapath *dp, struct ionic_qp_ring *q,
                           uint32_t qp_id, uint32_t slot)
{
    uint32_t stride = 1u << q->sq_stride_log2;
    uint64_t run;
    uint64_t gpa = buf_gpa(&q->sq_buf, (uint64_t)slot * stride, &run);

    uint8_t wqe[256];
    uint32_t read_sz = stride < sizeof(wqe) ? stride : (uint32_t)sizeof(wqe);
    if (!run || run < read_sz || dp_dma_read(dp->vfu_ctx, gpa, wqe, read_sz) < 0)
        return;

    uint64_t wqe_id;
    memcpy(&wqe_id, wqe + 0, 8);

    uint8_t op = wqe[8];
    uint16_t flags;
    memcpy(&flags, wqe + 10, 2);
    flags = be16toh(flags);

    uint32_t imm_be;
    memcpy(&imm_be, wqe + 12, 4);

    struct dp_sge_list src;
    if (flags & IONIC_V1_FLAG_INL) {
        /* Inline data lives in the payload area itself; describe it as a
         * single lkey-0 SGE pointing at the WQE's own guest pages. */
        uint32_t inl_len;
        memcpy(&inl_len, wqe + WQE_SEND_LEN_OFF, 4);
        inl_len = be32toh(inl_len);
        src.count = 1;
        src.va[0] = gpa + WQE_PLD_OFF;
        src.len[0] = inl_len;
        src.lkey[0] = 0;
        src.total = inl_len;
    } else {
        parse_sges(wqe, stride, wqe[9], &src);
    }

    bool remote = q->ib_qp_type != 1 /* GSI */ && q->ib_qp_type != 4 /* UD */;
    uint32_t status = IONIC_STS_OK;

    switch (op) {
    case IONIC_V1_OP_SEND:
    case IONIC_V1_OP_SEND_INV:
    case IONIC_V1_OP_SEND_IMM: {
        uint8_t recv_op = op == IONIC_V1_OP_SEND_IMM ? CQE_RECV_OP_SEND_IMM
                          : op == IONIC_V1_OP_SEND_INV
                              ? CQE_RECV_OP_SEND_INV
                              : CQE_RECV_OP_SEND;

        uint32_t dst_id = q->dest_valid ? q->dest_qp_id : qp_id;
        struct ionic_qp_ring *dq =
            dst_id < dp->qp_count && dp->qp[dst_id].valid ? &dp->qp[dst_id]
                                                          : NULL;
        if (!dq) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u SEND to unknown peer %u", qp_id,
                    dst_id);
            break;
        }
        if (deliver_recv(dp, dq, dst_id, qp_id, &src, recv_op, imm_be) < 0)
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u has no posted receive, dropping "
                    "%u bytes from QP %u",
                    dst_id, src.total, qp_id);
        break;
    }

    case IONIC_V1_OP_RDMA_READ:
    case IONIC_V1_OP_RDMA_WRITE:
    case IONIC_V1_OP_RDMA_WRITE_IMM: {
        uint32_t va_hi, va_lo, rkey, length;
        memcpy(&va_hi, wqe + WQE_RDMA_VA_HI_OFF, 4);
        memcpy(&va_lo, wqe + WQE_RDMA_VA_LO_OFF, 4);
        memcpy(&rkey, wqe + WQE_RDMA_RKEY_OFF, 4);
        memcpy(&length, wqe + WQE_SEND_LEN_OFF, 4);

        uint64_t remote_va =
            ((uint64_t)be32toh(va_hi) << 32) | be32toh(va_lo);
        rkey = be32toh(rkey);
        length = be32toh(length);

        bool to_remote = op != IONIC_V1_OP_RDMA_READ;
        uint32_t moved = rdma_xfer(dp, &src, rkey, remote_va, length,
                                   to_remote);
        if (moved != length) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u RDMA %s rkey=%#x va=%#lx moved "
                    "%u of %u bytes",
                    qp_id, to_remote ? "write" : "read", rkey,
                    (unsigned long)remote_va, moved, length);
            status = IONIC_STS_REMOTE_ACC_ERR;
            break;
        }

        /* Only the _IMM form consumes a receive on the far side, and it does
         * so with no payload: the data already landed via the rkey. */
        if (op == IONIC_V1_OP_RDMA_WRITE_IMM) {
            uint32_t dst_id = q->dest_valid ? q->dest_qp_id : qp_id;
            struct ionic_qp_ring *dq =
                dst_id < dp->qp_count && dp->qp[dst_id].valid ? &dp->qp[dst_id]
                                                              : NULL;
            struct dp_sge_list none = {.count = 0, .total = 0};
            if (dq)
                deliver_recv(dp, dq, dst_id, qp_id, &none,
                             CQE_RECV_OP_RDMA_IMM, imm_be);
        }
        break;
    }

    case IONIC_V1_OP_ATOMIC_CS:
    case IONIC_V1_OP_ATOMIC_FA:
        if (!do_atomic(dp, wqe, op == IONIC_V1_OP_ATOMIC_CS)) {
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u atomic op=%u failed", qp_id, op);
            status = IONIC_STS_REMOTE_ACC_ERR;
        }
        break;

    default:
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u unsupported op=%u", qp_id, op);
        status = IONIC_STS_REMOTE_ACC_ERR;
        break;
    }

    /*
     * Send-side completion.  RC/UC "remote" work requests are retired by a
     * running MSN sequence; everything else by the SQ index (NPG).  The driver
     * needs the MSN for every remote WQE, signalled or not, so it can advance
     * sq_msn_cons; NPG CQEs are only useful when the WQE asked to be signalled.
     */
    if (remote) {
        q->msn++;
        cq_post_send_msn(dp, q->sq_cq_id, qp_id, q->msn, status);
    } else if (flags & IONIC_V1_FLAG_SIG) {
        cq_post_send_npg(dp, q->sq_cq_id, qp_id, wqe_id);
    }
}

/* -------------------------------------------------------------------------
 * Doorbell handler
 *
 * Doorbell layout (struct ionic_doorbell, little-endian):
 *   le16 p_index   [0:1]
 *   u8   ring      [2]    — 0 = work, 1 = arm, 2 = arm solicited
 *   u8   qid_lo    [3]
 *   le16 qid_hi    [4:5]
 * -------------------------------------------------------------------------
 */

/* Hardware qtypes we advertise in Q_IDENTIFY (see ionic_eth_emu.c). */
#define DP_QTYPE_SQ 6
#define DP_QTYPE_RQ 7
#define DP_QTYPE_CQ 8

void ionic_datapath_doorbell(struct ionic_datapath *dp, int qtype,
                             uint64_t doorbell_val)
{
    uint16_t p_index = (uint16_t)(doorbell_val & 0xffffu);
    uint8_t ring = (uint8_t)((doorbell_val >> 16) & 0xffu);
    uint32_t qid = (uint32_t)(((doorbell_val >> 24) & 0xffu) |
                              (((doorbell_val >> 32) & 0xffffu) << 8));

    vfu_log(dp->vfu_ctx, LOG_DEBUG,
            "ionic_datapath: doorbell qtype=%d qid=%u ring=%u p_index=%u",
            qtype, qid, ring, p_index);

    switch (qtype) {
    case DP_QTYPE_CQ:
        if (qid < dp->cq_count && dp->cq[qid].valid && (ring == 1 || ring == 2))
            dp->cq[qid].armed = true;
        return;

    case DP_QTYPE_RQ:
        if (qid < dp->qp_count && dp->qp[qid].valid)
            dp->qp[qid].rq_prod += (uint32_t)(uint16_t)(
                p_index - (uint16_t)(dp->qp[qid].rq_prod & 0xffffu));
        return;

    case DP_QTYPE_SQ:
        break;

    default:
        return;
    }

    if (qid >= dp->qp_count || !dp->qp[qid].valid)
        return;

    struct ionic_qp_ring *q = &dp->qp[qid];

    /* Producer indices are 16-bit and free-running modulo the ring depth. */
    uint32_t target = (uint32_t)p_index % q->sq_depth;
    for (uint32_t n = 0; q->sq_cons % q->sq_depth != target && n < q->sq_depth;
         n++) {
        process_sq_wqe(dp, q, qid, q->sq_cons % q->sq_depth);
        q->sq_cons++;
    }
}
