/*
 * ionic_adminq.c — ionic RDMA admin queue service layer
 *
 * After the RDMA bootstrap (devcmds 50-53), ionic_rdma.ko posts
 * ionic_v1_admin_wqe entries to the admin queue rings.  This layer:
 *
 *   1. Polls each registered AQ ring for new WQEs (producer index advance).
 *   2. Dispatches to the appropriate handler (create_cq, create_qp, etc.).
 *   3. Posts ionic_v1_cqe completions to the admin CQ.
 *   4. Rings the EQ to trigger an interrupt so the driver collects the CQE.
 *
 * Wire formats are defined in ionic_fw.h (kernel-tools pinned ref).
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
#include <sys/mman.h> /* PROT_READ, PROT_WRITE */

#include <vfio-user/libvfio-user.h>

#include "ionic_adminq.h"
#include "ionic_datapath.h"
#include "rocm_ernic_compat.h"

/* -------------------------------------------------------------------------
 * ionic_fw.h constants (keep in sync with pinned kernel ref)
 * -------------------------------------------------------------------------
 */

/* Admin WQE stride and header length */
#define ADMIN_WQE_STRIDE  64
#define ADMIN_WQE_HDR_LEN 4

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
};

/*
 * ionic_v1_admin_wqe header (4 bytes):
 *   u8  op
 *   u8  rsvd
 *   le16 len   (total byte length of cmd body)
 *
 * Followed by the opcode-specific command body.  A WQE may span multiple
 * 64-byte strides if len+4 > 64.
 *
 * ionic_v1_cqe (32 bytes, big-endian):
 *   For admin CQE type (IONIC_V1_CQE_TYPE_ADMIN = 0):
 *     le16 cmd_idx     — AQ consumer index this completes
 *     u8   cmd_op
 *     u8   rsvd[17]
 *     le16 old_sq_cindex
 *     le16 old_rq_cq_cindex
 *   Followed by (at byte 28):
 *     be32 status_length   (status in high byte when error bit set)
 *     be32 qid_type_flags
 *       bit 0:  color
 *       bit 1:  error
 *       bits[7:5]: type  (0 = admin)
 *       bits[31:8]: qid
 */
#define CQE_SIZE 32

/* Color bit in qid_type_flags (big-endian byte [3] bit 0). */
#define CQE_COLOR_BIT 0x01u
#define CQE_ERROR_BIT 0x02u
/* Type field bits[7:5] = 0 means admin CQE; we always write 0 there. */

/* -------------------------------------------------------------------------
 * Per-AQ ring state
 * -------------------------------------------------------------------------
 */
#define MAX_AQ     4
#define MAX_CQ_MAP 256
#define MAX_QP_MAP 256
#define MAX_MR_MAP 256

/* enum ionic_mrf_bits: the low 12 bits are IB access flags. */
#define IONIC_MRF_ACCESS_MASK 0x0fffu

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

    /* Raises an EQ event once a CQE has been written. */
    ionic_adminq_cq_event_fn_t cq_event_fn;
    void *cq_event_opaque;

    /* The driver names CQs by its own cqid; rdma_rm hands back its own
     * handles.  CREATE_QP references CQs by cqid, so keep the translation. */
    struct {
        bool valid;
        uint32_t cq_id;
        uint32_t handle;
    } cq_map[MAX_CQ_MAP];

    /* Same translation for QPs: the driver's qpid vs the backend's QPN. */
    struct {
        bool valid;
        uint32_t qp_id;
        uint32_t qpn;
    } qp_map[MAX_QP_MAP];

    /* And for MRs: the driver's mrid vs the backend's handle. */
    struct {
        bool valid;
        uint32_t mr_id;
        uint32_t handle;
    } mr_map[MAX_MR_MAP];

    /* ionic allocates protection domains entirely in the driver, so no PD
     * ever reaches us.  rdma_rm still needs one, so share a single backend PD
     * across every QP. */
    bool pd_valid;
    uint32_t pd_handle;

    /* Data path that owns the SQ/RQ/CQ rings these commands describe. */
    struct ionic_datapath *dp;
};

void ionic_adminq_set_datapath(struct ionic_adminq_ctx *ctx,
                               struct ionic_datapath *dp)
{
    if (ctx)
        ctx->dp = dp;
}

static int adminq_get_pd(struct ionic_adminq_ctx *ctx, uint32_t *pd_handle)
{
    if (!ctx->pd_valid) {
        int ret = ionic_rm_alloc_pd(ctx->pvrdma_handle, &ctx->pd_handle);
        if (ret)
            return ret;
        ctx->pd_valid = true;
        vfu_log(ctx->vfu_ctx, LOG_INFO, "ionic_adminq: shared PD handle=%u",
                ctx->pd_handle);
    }
    *pd_handle = ctx->pd_handle;
    return 0;
}

static void adminq_map_cq(struct ionic_adminq_ctx *ctx, uint32_t cq_id,
                          uint32_t handle)
{
    for (int i = 0; i < MAX_CQ_MAP; i++) {
        if (!ctx->cq_map[i].valid || ctx->cq_map[i].cq_id == cq_id) {
            ctx->cq_map[i] = (typeof(ctx->cq_map[0])){
                .valid = true, .cq_id = cq_id, .handle = handle};
            return;
        }
    }
    vfu_log(ctx->vfu_ctx, LOG_WARNING, "ionic_adminq: CQ map full, cq_id=%u",
            cq_id);
}

static bool adminq_lookup_cq(struct ionic_adminq_ctx *ctx, uint32_t cq_id,
                             uint32_t *handle)
{
    for (int i = 0; i < MAX_CQ_MAP; i++) {
        if (ctx->cq_map[i].valid && ctx->cq_map[i].cq_id == cq_id) {
            *handle = ctx->cq_map[i].handle;
            return true;
        }
    }
    return false;
}

static void adminq_map_qp(struct ionic_adminq_ctx *ctx, uint32_t qp_id,
                          uint32_t qpn)
{
    for (int i = 0; i < MAX_QP_MAP; i++) {
        if (!ctx->qp_map[i].valid || ctx->qp_map[i].qp_id == qp_id) {
            ctx->qp_map[i] = (typeof(ctx->qp_map[0])){
                .valid = true, .qp_id = qp_id, .qpn = qpn};
            return;
        }
    }
    vfu_log(ctx->vfu_ctx, LOG_WARNING, "ionic_adminq: QP map full, qp_id=%u",
            qp_id);
}

static bool adminq_lookup_qp(struct ionic_adminq_ctx *ctx, uint32_t qp_id,
                             uint32_t *qpn)
{
    for (int i = 0; i < MAX_QP_MAP; i++) {
        if (ctx->qp_map[i].valid && ctx->qp_map[i].qp_id == qp_id) {
            *qpn = ctx->qp_map[i].qpn;
            return true;
        }
    }
    return false;
}

static void adminq_unmap_qp(struct ionic_adminq_ctx *ctx, uint32_t qp_id)
{
    for (int i = 0; i < MAX_QP_MAP; i++)
        if (ctx->qp_map[i].valid && ctx->qp_map[i].qp_id == qp_id)
            ctx->qp_map[i].valid = false;
}

static void adminq_map_mr(struct ionic_adminq_ctx *ctx, uint32_t mr_id,
                          uint32_t handle)
{
    for (int i = 0; i < MAX_MR_MAP; i++) {
        if (!ctx->mr_map[i].valid || ctx->mr_map[i].mr_id == mr_id) {
            ctx->mr_map[i] = (typeof(ctx->mr_map[0])){
                .valid = true, .mr_id = mr_id, .handle = handle};
            return;
        }
    }
    vfu_log(ctx->vfu_ctx, LOG_WARNING, "ionic_adminq: MR map full, mr_id=%u",
            mr_id);
}

static bool adminq_lookup_mr(struct ionic_adminq_ctx *ctx, uint32_t mr_id,
                             uint32_t *handle)
{
    for (int i = 0; i < MAX_MR_MAP; i++) {
        if (ctx->mr_map[i].valid && ctx->mr_map[i].mr_id == mr_id) {
            *handle = ctx->mr_map[i].handle;
            return true;
        }
    }
    return false;
}

static void adminq_unmap_mr(struct ionic_adminq_ctx *ctx, uint32_t mr_id)
{
    for (int i = 0; i < MAX_MR_MAP; i++)
        if (ctx->mr_map[i].valid && ctx->mr_map[i].mr_id == mr_id)
            ctx->mr_map[i].valid = false;
}

static void adminq_unmap_cq(struct ionic_adminq_ctx *ctx, uint32_t cq_id)
{
    for (int i = 0; i < MAX_CQ_MAP; i++)
        if (ctx->cq_map[i].valid && ctx->cq_map[i].cq_id == cq_id)
            ctx->cq_map[i].valid = false;
}

/* -------------------------------------------------------------------------
 * Construction / destruction
 * -------------------------------------------------------------------------
 */

struct ionic_adminq_ctx *ionic_adminq_create(vfu_ctx_t *vfu_ctx)
{
    struct ionic_adminq_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->vfu_ctx = vfu_ctx;
    return ctx;
}

void ionic_adminq_destroy(struct ionic_adminq_ctx *ctx)
{
    free(ctx);
}

void ionic_adminq_register_queue(struct ionic_adminq_ctx *ctx, int aq_idx,
                                 uint64_t aq_dma, uint8_t aq_depth_log2,
                                 uint64_t cq_dma, uint8_t cq_depth_log2,
                                 uint32_t cq_id, uint32_t eq_id)
{
    if (!ctx || aq_idx < 0 || aq_idx >= MAX_AQ)
        return;

    struct ionic_aq_ring *r = &ctx->aq[aq_idx];
    r->valid = true;
    r->aq_dma = aq_dma;
    r->aq_depth = 1u << aq_depth_log2;
    r->aq_cons = 0;
    r->aq_prod = 0;
    r->cq_dma = cq_dma;
    r->cq_depth = 1u << cq_depth_log2;
    r->cq_prod = 0;
    r->cq_color = true; /* ionic: initial color = true */
    r->aq_id = (uint32_t)aq_idx;
    r->cq_id = cq_id;
    r->eq_id = eq_id;

    if (aq_idx >= ctx->aq_count)
        ctx->aq_count = aq_idx + 1;

    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq: registered AQ[%d] aq_dma=%#lx depth=%u "
            "cq_dma=%#lx cq_depth=%u cq_id=%u eq_id=%u",
            aq_idx, aq_dma, r->aq_depth, cq_dma, r->cq_depth, cq_id, eq_id);
}

void ionic_adminq_set_resources(struct ionic_adminq_ctx *ctx, void *dev_res,
                                void *backend_dev)
{
    /* dev_res and backend_dev are not stored directly — we use the
     * ionic_rm_* compat wrappers via pvrdma_handle instead. */
    (void)dev_res;
    (void)backend_dev;
    /* pvrdma_handle must be set separately via ionic_adminq_set_pvrdma. */
}

void ionic_adminq_set_cq_event_cb(struct ionic_adminq_ctx *ctx,
                                  ionic_adminq_cq_event_fn_t fn, void *opaque)
{
    if (ctx) {
        ctx->cq_event_fn = fn;
        ctx->cq_event_opaque = opaque;
    }
}

void ionic_adminq_set_pvrdma(struct ionic_adminq_ctx *ctx, void *handle)
{
    if (ctx)
        ctx->pvrdma_handle = (pvrdma_handle_t)handle;
}

void ionic_adminq_update_prod(struct ionic_adminq_ctx *ctx, int aq_idx,
                              uint16_t p_index)
{
    if (!ctx || aq_idx < 0 || aq_idx >= ctx->aq_count)
        return;
    struct ionic_aq_ring *r = &ctx->aq[aq_idx];
    if (r->valid)
        r->aq_prod = (uint32_t)p_index % r->aq_depth;
}

/* -------------------------------------------------------------------------
 * DMA helpers
 * -------------------------------------------------------------------------
 */

static int dma_read(vfu_ctx_t *vfu_ctx, uint64_t gpa, void *buf, size_t len)
{
    dma_sg_t *sg = malloc(dma_sg_size());
    struct iovec iov;
    int ret;

    if (!sg)
        return -ENOMEM;

    ret = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)(uintptr_t)gpa, len, sg, 1,
                          PROT_READ);
    if (ret < 0) {
        free(sg);
        return ret;
    }

    ret = vfu_sgl_get(vfu_ctx, sg, &iov, 1, 0);
    if (ret < 0) {
        free(sg);
        return ret;
    }

    memcpy(buf, iov.iov_base, len);
    vfu_sgl_put(vfu_ctx, sg, &iov, 1);
    free(sg);
    return 0;
}

static int dma_write(vfu_ctx_t *vfu_ctx, uint64_t gpa, const void *buf,
                     size_t len)
{
    dma_sg_t *sg = malloc(dma_sg_size());
    struct iovec iov;
    int ret;

    if (!sg)
        return -ENOMEM;

    ret = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)(uintptr_t)gpa, len, sg, 1,
                          PROT_WRITE);
    if (ret < 0) {
        free(sg);
        return ret;
    }

    ret = vfu_sgl_get(vfu_ctx, sg, &iov, 1, 0);
    if (ret < 0) {
        free(sg);
        return ret;
    }

    memcpy(iov.iov_base, buf, len);
    vfu_sgl_mark_dirty(vfu_ctx, sg, 1);
    vfu_sgl_put(vfu_ctx, sg, &iov, 1);
    free(sg);
    return 0;
}

/* -------------------------------------------------------------------------
 * RoCE header template
 * -------------------------------------------------------------------------
 */

/*
 * CREATE_AH and the RTR transition of MODIFY_QP both hand us a packet header
 * template built by ib_ud_header_pack() with the BTH and DETH trimmed off:
 * Ethernet, an optional 802.1Q tag, IPv4 or IPv6, then UDP.  The destination
 * address in it is the only statement of which peer the QP is being pointed
 * at -- the WQEs themselves never name one.
 *
 * Fills @dgid with the destination GID (IPv4 in the ::ffff:a.b.c.d mapped
 * form the rest of the stack expects) and @dmac with the destination MAC.
 * Returns 1 on success, 0 if the template could not be parsed.
 */
static int adminq_parse_roce_hdr(struct ionic_adminq_ctx *ctx, uint64_t gpa,
                                 uint32_t len, uint8_t dgid[16],
                                 uint8_t dmac[6])
{
    uint8_t hdr[128];

    if (len < 14 || len > sizeof(hdr))
        return 0;
    if (dma_read(ctx->vfu_ctx, gpa, hdr, len) < 0)
        return 0;

    memcpy(dmac, hdr, 6);

    uint32_t off = 12;
    uint16_t ethertype = (uint16_t)((hdr[off] << 8) | hdr[off + 1]);

    if (ethertype == 0x8100) { /* 802.1Q tag, ethertype repeats after it */
        off += 4;
        if (off + 2 > len)
            return 0;
        ethertype = (uint16_t)((hdr[off] << 8) | hdr[off + 1]);
    }
    off += 2;

    if (ethertype == 0x0800) { /* IPv4: daddr at +16 */
        if (off + 20 > len)
            return 0;
        memset(dgid, 0, 16);
        dgid[10] = 0xff;
        dgid[11] = 0xff;
        memcpy(dgid + 12, hdr + off + 16, 4);
        return 1;
    }

    if (ethertype == 0x86dd) { /* IPv6: daddr at +24 */
        if (off + 40 > len)
            return 0;
        memcpy(dgid, hdr + off + 24, 16);
        return 1;
    }

    vfu_log(ctx->vfu_ctx, LOG_WARNING,
            "ionic_adminq: unrecognised RoCE header ethertype %#x", ethertype);
    return 0;
}

/* -------------------------------------------------------------------------
 * Post an admin CQE
 * -------------------------------------------------------------------------
 */

static void post_admin_cqe(struct ionic_adminq_ctx *ctx,
                           struct ionic_aq_ring *r, uint16_t cmd_idx,
                           uint8_t cmd_op, uint8_t status)
{
    /* ionic_v1_cqe layout for admin completions (32 bytes, see ionic_fw.h):
     *   [0:1]   be16 cmd_idx  — AQ consumer index this completes
     *   [2]     u8   cmd_op
     *   [3:19]  u8   rsvd[17]
     *   [20:21] le16 old_sq_cindex
     *   [22:23] le16 old_rq_cq_cindex
     *   [24:27] be32 status_length  (status in bits[31:24] when error set)
     *   [28:31] be32 qid_type_flags (color[0], error[1], type[7:5], qid[31:8])
     */
    uint8_t cqe[CQE_SIZE];
    memset(cqe, 0, CQE_SIZE);

    uint16_t ci = htobe16(cmd_idx);
    memcpy(cqe + 0, &ci, 2);
    cqe[2] = cmd_op;

    uint32_t sl = htobe32(status ? ((uint32_t)status << 24) : 0);
    memcpy(cqe + 24, &sl, 4);

    /* qid_type_flags: bit 0 = color, bit 1 = error, bits[7:5] = type
     * (0 = admin), bits[31:8] = the AQ id, which the driver checks against
     * aq->aqid before accepting the completion. */
    uint32_t qtf = (r->aq_id << 8) |
                   (uint32_t)(r->cq_color ? CQE_COLOR_BIT : 0) |
                   (status ? CQE_ERROR_BIT : 0);
    qtf = htobe32(qtf);
    memcpy(cqe + 28, &qtf, 4);

    /* Write CQE to guest memory */
    uint64_t cqe_gpa =
        r->cq_dma + (uint64_t)(r->cq_prod % r->cq_depth) * CQE_SIZE;
    if (dma_write(ctx->vfu_ctx, cqe_gpa, cqe, CQE_SIZE) < 0) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq: failed to write CQE to %#lx", cqe_gpa);
        return;
    }

    vfu_log(ctx->vfu_ctx, LOG_DEBUG,
            "ionic_adminq: CQE aq_id=%u cq_id=%u eq_id=%u cmd_idx=%u op=%u "
            "prod=%u color=%u gpa=%#lx",
            r->aq_id, r->cq_id, r->eq_id, cmd_idx, cmd_op, r->cq_prod,
            (unsigned int)r->cq_color, cqe_gpa);

    r->cq_prod++;
    if (r->cq_prod % r->cq_depth == 0)
        r->cq_color = !r->cq_color; /* color flips on ring wrap */

    /* The driver only drains an admin CQ from ionic_poll_eq(), so the CQE is
     * invisible until an EQE and its interrupt follow it. */
    if (ctx->cq_event_fn)
        ctx->cq_event_fn(ctx->cq_event_opaque, r->eq_id, r->cq_id);
}

/* -------------------------------------------------------------------------
 * WQE dispatch
 * -------------------------------------------------------------------------
 */

/* -------------------------------------------------------------------------
 * ionic_admin_create_cq body (34 bytes from ionic_fw.h):
 *   le32 eq_id        [0:3]
 *   u8   depth_log2   [4]
 *   u8   stride_log2  [5]
 *   u8   dir_size_... [6]
 *   u8   page_sz_log2 [7]
 *   le32 cq_flags     [8:11]
 *   le32 id_ver       [12:15]  (cqid | ver<<24)
 *   le32 tbl_index    [16:19]
 *   le32 map_count    [20:23]
 *   le64 dma_addr     [24:31]
 *   le16 dbid_flags   [32:33]
 * -------------------------------------------------------------------------
 */
static uint8_t handle_create_cq_op(struct ionic_adminq_ctx *ctx,
                                   const uint8_t *body, uint16_t len)
{
    if (!ctx->pvrdma_handle) {
        vfu_log(ctx->vfu_ctx, LOG_WARNING,
                "ionic_adminq CREATE_CQ: no pvrdma_handle (stub ok)");
        return 0;
    }
    if (len < 34) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_CQ: short body (%u)", len);
        return 1;
    }

    uint32_t id_ver;
    memcpy(&id_ver, body + 12, 4);
    id_ver = le32toh(id_ver);
    uint32_t cq_id = id_ver & 0x00ffffffu;

    uint32_t cq_handle;
    int ret = ionic_rm_alloc_cq(ctx->pvrdma_handle,
                                1u << body[4], /* cqe count = 2^depth */
                                &cq_handle);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_CQ %u: rdma_rm_alloc_cq failed (%d)",
                cq_id, ret);
        return 1;
    }

    adminq_map_cq(ctx, cq_id, cq_handle);

    /* ionic_admin_create_cq: eq_id[0:3], depth_log2[4], stride_log2[5],
     * dir_size_log2[6], page_size_log2[7], cq_flags[8:11], id_ver[12:15],
     * tbl_index[16:19], map_count[20:23], dma_addr[24:31], dbid_flags[32:33] */
    uint32_t eq_id, map_count;
    uint64_t dma_addr;
    memcpy(&eq_id, body + 0, 4);
    memcpy(&map_count, body + 20, 4);
    memcpy(&dma_addr, body + 24, 8);

    struct ionic_dp_ring_desc ring = {
        .buf = {.dma_addr = le64toh(dma_addr),
                .map_count = le32toh(map_count),
                .page_size_log2 = body[7]},
        .depth_log2 = body[4],
        .stride_log2 = body[5],
    };
    ionic_datapath_register_cq(ctx->dp, cq_id, le32toh(eq_id), &ring);

    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq CREATE_CQ cq_id=%u handle=%u depth=2^%u", cq_id,
            cq_handle, body[4]);
    return 0;
}

/* -------------------------------------------------------------------------
 * ionic_admin_create_qp body (64 bytes from ionic_fw.h):
 *   le32 pd_id        [0:3]
 *   be32 priv_flags   [4:7]
 *   le32 sq_cq_id     [8:11]
 *   u8   sq_depth_log2  [12]
 *   u8   sq_stride_log2 [13]
 *   u8   sq_dir_size  [14]
 *   u8   sq_page_size [15]
 *   le32 sq_tbl_index [16:19]
 *   le32 sq_map_count [20:23]
 *   le64 sq_dma_addr  [24:31]
 *   le32 rq_cq_id     [32:35]
 *   u8   rq_depth_log2  [36]
 *   u8   rq_stride_log2 [37]
 *   u8   rq_dir_size  [38]
 *   u8   rq_page_size [39]
 *   le32 rq_tbl_index [40:43]
 *   le32 rq_map_count [44:47]
 *   le64 rq_dma_addr  [48:55]
 *   le32 id_ver       [56:59]  (qpid | ver<<24)
 *   le16 dbid_flags   [60:61]
 *   u8   type_state   [62]     qp_type | (state<<4)
 * -------------------------------------------------------------------------
 */
/* enum ionic_qp_type (RC=0, UC=1, RD=2, UD=3, ...) is not enum ib_qp_type
 * (SMI=0, GSI=1, RC=2, UC=3, UD=4), and rdma_rm speaks the latter.  The driver
 * also folds GSI into UD, but reserves qpid 1 (== IB_QPT_GSI) for it, which is
 * the only way to tell the two apart on the wire. */
static uint8_t ionic_qpt_to_ib(uint8_t ionic_qpt, uint32_t qp_id)
{
    if (qp_id == 1)
        return 1; /* IB_QPT_GSI */
    switch (ionic_qpt) {
    case 0:
        return 2; /* IB_QPT_RC */
    case 1:
        return 3; /* IB_QPT_UC */
    case 3:
        return 4; /* IB_QPT_UD */
    default:
        return 2;
    }
}

static uint8_t handle_create_qp_op(struct ionic_adminq_ctx *ctx,
                                   const uint8_t *body, uint16_t len)
{
    if (!ctx->pvrdma_handle) {
        vfu_log(ctx->vfu_ctx, LOG_WARNING,
                "ionic_adminq CREATE_QP: no pvrdma_handle (stub ok)");
        return 0;
    }
    if (len < 60) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP: short body (%u)", len);
        return 1;
    }

    uint32_t sq_cq_id, rq_cq_id, id_ver;
    memcpy(&sq_cq_id, body + 8, 4);
    memcpy(&rq_cq_id, body + 32, 4);
    memcpy(&id_ver, body + 56, 4);
    sq_cq_id = le32toh(sq_cq_id);
    rq_cq_id = le32toh(rq_cq_id);
    uint32_t qp_id = le32toh(id_ver) & 0x00ffffffu;

    uint8_t type_state = body[62];
    uint8_t ib_qp_type = ionic_qpt_to_ib(type_state & 0x0fu, qp_id);

    uint32_t send_cq, recv_cq;
    if (!adminq_lookup_cq(ctx, sq_cq_id, &send_cq) ||
        !adminq_lookup_cq(ctx, rq_cq_id, &recv_cq)) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP %u: unknown cq (sq=%u rq=%u)", qp_id,
                sq_cq_id, rq_cq_id);
        return 1;
    }

    uint32_t pd_handle;
    int ret = adminq_get_pd(ctx, &pd_handle);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP %u: rdma_rm_alloc_pd failed (%d)",
                qp_id, ret);
        return 1;
    }

    uint32_t qpn;
    ret = ionic_rm_alloc_qp(ctx->pvrdma_handle, pd_handle, ib_qp_type,
                            (uint32_t)(1u << body[12]), /* max_send_wr */
                            (uint32_t)(1u << body[36]), /* max_recv_wr */
                            send_cq, recv_cq, &qpn);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP %u: rdma_rm_alloc_qp failed (%d)",
                qp_id, ret);
        return 1;
    }

    adminq_map_qp(ctx, qp_id, qpn);

    uint32_t sq_map_count, rq_map_count;
    uint64_t sq_dma, rq_dma;
    memcpy(&sq_map_count, body + 20, 4);
    memcpy(&sq_dma, body + 24, 8);
    memcpy(&rq_map_count, body + 44, 4);
    memcpy(&rq_dma, body + 48, 8);

    struct ionic_dp_ring_desc sq = {
        .buf = {.dma_addr = le64toh(sq_dma),
                .map_count = le32toh(sq_map_count),
                .page_size_log2 = body[15]},
        .depth_log2 = body[12],
        .stride_log2 = body[13],
    };
    struct ionic_dp_ring_desc rq = {
        .buf = {.dma_addr = le64toh(rq_dma),
                .map_count = le32toh(rq_map_count),
                .page_size_log2 = body[39]},
        .depth_log2 = body[36],
        .stride_log2 = body[37],
    };
    ionic_datapath_register_qp(ctx->dp, qp_id, ib_qp_type, sq_cq_id, &sq,
                               rq_cq_id, &rq);

    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq CREATE_QP qp_id=%u qpn=%u type=%u sq_cq=%u/%u "
            "rq_cq=%u/%u pd=%u",
            qp_id, qpn, ib_qp_type, sq_cq_id, send_cq, rq_cq_id, recv_cq,
            pd_handle);
    return 0;
}

/* -------------------------------------------------------------------------
 * WQE dispatch
 * -------------------------------------------------------------------------
 */

static uint8_t dispatch_wqe(struct ionic_adminq_ctx *ctx, uint8_t op,
                            const uint8_t *body, uint16_t len)
{
    vfu_log(ctx->vfu_ctx, LOG_INFO, "ionic_adminq: WQE op=%u len=%u", op, len);

    switch (op) {
    case IONIC_V1_ADMIN_NOOP:
        return 0;

    case IONIC_V1_ADMIN_CREATE_CQ:
        return handle_create_cq_op(ctx, body, len);

    case IONIC_V1_ADMIN_CREATE_QP:
        return handle_create_qp_op(ctx, body, len);

    case IONIC_V1_ADMIN_CREATE_MR: {
        if (len < 45 || !ctx->pvrdma_handle) {
            return 0;
        }
        /* ionic_admin_create_mr body (45 bytes):
         *   le64 va         [0:7]
         *   le64 length     [8:15]
         *   le32 pd_id      [16:19]
         *   le32 id_ver     [20:23]  (mrid | ver<<24)
         *   le32 tbl_index  [24:27]
         *   le32 map_count  [28:31]
         *   le64 dma_addr   [32:39]
         *   le16 dbid_flags [40:41]   IONIC_MRF_* — low 12 bits are the
         *                             IB access flags, same bit positions
         *   u8   pt_type    [42]
         *   u8   dir_size_log2  [43]
         *   u8   page_size_log2 [44] */
        uint32_t id_ver;
        memcpy(&id_ver, body + 20, 4);
        /* The full mrid, key byte included, is what userspace puts in an SGE
         * lkey; the low 24 bits alone are the id we track resources by. */
        uint32_t mrid = le32toh(id_ver);
        uint32_t mr_id = mrid & 0x00ffffffu;

        uint16_t mrf;
        memcpy(&mrf, body + 40, 2);
        uint32_t access = le16toh(mrf) & IONIC_MRF_ACCESS_MASK;

        uint32_t pd_handle;
        int ret = adminq_get_pd(ctx, &pd_handle);
        if (ret) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq CREATE_MR %u: rdma_rm_alloc_pd failed (%d)",
                    mr_id, ret);
            return 1;
        }

        uint32_t mr_handle;
        ret = ionic_rm_alloc_mr(ctx->pvrdma_handle, pd_handle, access,
                                &mr_handle);
        if (ret) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq CREATE_MR %u: failed (%d)", mr_id, ret);
            return 1;
        }

        adminq_map_mr(ctx, mr_id, mr_handle);

        uint64_t va, length, dma_addr;
        uint32_t map_count;
        memcpy(&va, body + 0, 8);
        memcpy(&length, body + 8, 8);
        memcpy(&map_count, body + 28, 4);
        memcpy(&dma_addr, body + 32, 8);

        struct ionic_dp_buf_desc mrbuf = {.dma_addr = le64toh(dma_addr),
                                          .map_count = le32toh(map_count),
                                          .page_size_log2 = body[44]};
        ionic_datapath_register_mr(ctx->dp, mrid, le64toh(va), le64toh(length),
                                   &mrbuf);

        vfu_log(ctx->vfu_ctx, LOG_INFO,
                "ionic_adminq CREATE_MR mr_id=%u handle=%u access=%#x pd=%u",
                mr_id, mr_handle, access, pd_handle);
        return 0;
    }

    case IONIC_V1_ADMIN_DESTROY_MR: {
        if (len < 4 || !ctx->pvrdma_handle) {
            return 0;
        }
        uint32_t mrid, mr_handle;
        memcpy(&mrid, body, 4);
        mrid = le32toh(mrid);
        uint32_t mr_id = mrid & 0x00ffffffu;
        ionic_datapath_unregister_mr(ctx->dp, mrid);
        if (!adminq_lookup_mr(ctx, mr_id, &mr_handle))
            return 0;
        ionic_rm_dealloc_mr(ctx->pvrdma_handle, mr_handle);
        adminq_unmap_mr(ctx, mr_id);
        return 0;
    }

    case IONIC_V1_ADMIN_DESTROY_CQ: {
        if (len < 4 || !ctx->pvrdma_handle) {
            return 0;
        }
        uint32_t cq_id, cq_handle;
        memcpy(&cq_id, body, 4);
        cq_id = le32toh(cq_id);
        ionic_datapath_unregister_cq(ctx->dp, cq_id);
        if (!adminq_lookup_cq(ctx, cq_id, &cq_handle))
            return 0; /* never created here; nothing to release */
        ionic_rm_dealloc_cq(ctx->pvrdma_handle, cq_handle);
        adminq_unmap_cq(ctx, cq_id);
        return 0;
    }

    case IONIC_V1_ADMIN_DESTROY_QP: {
        if (len < 4 || !ctx->pvrdma_handle) {
            return 0;
        }
        uint32_t qp_id, qpn;
        memcpy(&qp_id, body, 4);
        qp_id = le32toh(qp_id);
        ionic_datapath_unregister_qp(ctx->dp, qp_id);
        if (!adminq_lookup_qp(ctx, qp_id, &qpn))
            return 0;
        ionic_rm_dealloc_qp(ctx->pvrdma_handle, qpn);
        adminq_unmap_qp(ctx, qp_id);
        return 0;
    }

    case IONIC_V1_ADMIN_MODIFY_QP: {
        /* ionic_admin_mod_qp body (60 bytes):
         *   be32 attr_mask    [0:3]
         *   u8   dcqcn        [4]
         *   u8   tfp          [5]
         *   be16 access_flags [6:7]
         *   le32 rq_psn       [8:11]
         *   le32 sq_psn       [12:15]
         *   le32 qkey_dest_qpn[16:19]
         *   le32 rate_limit   [20:23]
         *   u8   pmtu         [24]
         *   u8   retry        [25]
         *   u8   rnr_timer    [26]
         *   u8   retry_timeout[27]
         *   u8   rsq_depth    [28]
         *   u8   rrq_depth    [29]
         *   le16 pkey_id      [30:31]
         *   le32 ah_id_len    [32:35]
         *   u8   en_pcp       [36]
         *   u8   ip_dscp      [37]
         *   u8   rsvd2        [38]
         *   u8   type_state   [39]
         *   le32 rrq_index    [40:43]
         *   le32 rsq_index    [44:47]
         *   le64 dma_addr     [48:55]  (AH DMA if ah_id_len != 0)
         *   le32 id_ver       [56:59]  (qpn | ver<<24)
         */
        if (len < 60 || !ctx->pvrdma_handle) {
            return 0;
        }

        uint32_t attr_mask_be;
        memcpy(&attr_mask_be, body + 0, 4);
        uint32_t attr_mask = be32toh(attr_mask_be);

        uint32_t rq_psn, sq_psn, qkey_dest_qpn, id_ver;
        memcpy(&rq_psn, body + 8, 4);
        rq_psn = le32toh(rq_psn);
        memcpy(&sq_psn, body + 12, 4);
        sq_psn = le32toh(sq_psn);
        memcpy(&qkey_dest_qpn, body + 16, 4);
        qkey_dest_qpn = le32toh(qkey_dest_qpn);
        memcpy(&id_ver, body + 56, 4);
        id_ver = le32toh(id_ver);

        uint8_t type_state = body[39];
        uint32_t qp_id = id_ver & 0x00ffffffu;

        uint32_t qpn;
        if (!adminq_lookup_qp(ctx, qp_id, &qpn)) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq MODIFY_QP: unknown qp_id=%u", qp_id);
            return 1;
        }

        /* The RoCE header template the driver DMAs for this transition is the
         * only place the destination address appears; without it every peer
         * resolves to the same default node. */
        uint32_t ah_id_len;
        bool dgid_valid = false;
        uint64_t ah_dma;
        uint8_t dgid[16] = {0};
        uint8_t dmac[6] = {0};

        memcpy(&ah_id_len, body + 32, 4);
        ah_id_len = le32toh(ah_id_len);
        memcpy(&ah_dma, body + 48, 8);
        ah_dma = le64toh(ah_dma);

        uint32_t hdr_len = ah_id_len >> 24;
        if (hdr_len && ah_dma)
            dgid_valid = adminq_parse_roce_hdr(ctx, ah_dma, hdr_len, dgid, dmac);

        if (dgid_valid)
            vfu_log(ctx->vfu_ctx, LOG_DEBUG,
                    "ionic_adminq MODIFY_QP %u: peer %02x:%02x:%02x:%02x:%02x:"
                    "%02x gid %02x%02x:%02x%02x:%02x%02x:%02x%02x",
                    qp_id, dmac[0], dmac[1], dmac[2], dmac[3], dmac[4], dmac[5],
                    dgid[8], dgid[9], dgid[10], dgid[11], dgid[12], dgid[13],
                    dgid[14], dgid[15]);

        /* IB_QP_DEST_QPN: the driver names the peer by its own qpid, but the
         * backend routes on its QPNs.  For UD this field is the qkey instead,
         * so only translate when the mask says it is a destination QPN. */
        if (attr_mask & (1u << 20)) {
            uint32_t dest_node = ionic_dp_node_from_gid(
                ctx->dp, dgid_valid ? dgid : NULL);

            ionic_datapath_set_dest(ctx->dp, qp_id, qkey_dest_qpn, dest_node);
            uint32_t dest_qpn;
            if (adminq_lookup_qp(ctx, qkey_dest_qpn, &dest_qpn))
                qkey_dest_qpn = dest_qpn;
        }

        int ret =
            ionic_rm_modify_qp(ctx->pvrdma_handle, qpn, attr_mask, type_state,
                               sq_psn, rq_psn, qkey_dest_qpn, dgid);
        if (ret) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq MODIFY_QP %u: failed (%d)", qp_id, ret);
            return 1;
        }
        vfu_log(ctx->vfu_ctx, LOG_INFO,
                "ionic_adminq MODIFY_QP qp_id=%u qpn=%u type_state=%#x "
                "attr=%#x",
                qp_id, qpn, type_state, attr_mask);
        return 0;
    }

    case IONIC_V1_ADMIN_QUERY_QP:
    case IONIC_V1_ADMIN_CREATE_AH:
    case IONIC_V1_ADMIN_QUERY_AH:
    case IONIC_V1_ADMIN_DESTROY_AH:
        /* Remaining opcodes: stub */
        (void)body;
        (void)len;
        vfu_log(ctx->vfu_ctx, LOG_WARNING,
                "ionic_adminq: op=%u stub (succeeds)", op);
        return 0;

    case IONIC_V1_ADMIN_STATS_HDRS:
    case IONIC_V1_ADMIN_STATS_VALS:
    case IONIC_V1_ADMIN_QP_STATS_HDRS:
    case IONIC_V1_ADMIN_QP_STATS_VALS:
    case IONIC_V1_ADMIN_MODIFY_DCQCN:
    case IONIC_V1_ADMIN_DEBUG:
        return 0; /* benign stub */

    default:
        vfu_log(ctx->vfu_ctx, LOG_WARNING, "ionic_adminq: unknown op=%u", op);
        return 1; /* IONIC_RC_ENOSUPP */
    }
}

/* -------------------------------------------------------------------------
 * Poll loop — called from server main loop
 * -------------------------------------------------------------------------
 */

void ionic_adminq_poll(struct ionic_adminq_ctx *ctx, vfu_ctx_t *vfu_ctx)
{
    if (!ctx)
        return;

    for (int i = 0; i < ctx->aq_count; i++) {
        struct ionic_aq_ring *r = &ctx->aq[i];
        if (!r->valid)
            continue;

        /* Process the strides between aq_cons and aq_prod (set by doorbell
         * writes).  The producer index is the only correct way to detect
         * pending work: op==0 (NOOP) with len==0 is a valid WQE, not an empty
         * slot marker, and stale zeroed slots after a ring wrap are
         * indistinguishable from unwritten ones. */
        if (r->aq_prod == r->aq_cons)
            continue;

        uint32_t pending =
            (r->aq_prod - r->aq_cons + r->aq_depth) % r->aq_depth;

        uint32_t processed = 0;
        while (processed < pending && processed < r->aq_depth) {
            uint32_t slot = r->aq_cons % r->aq_depth;
            uint64_t wqe_gpa = r->aq_dma + (uint64_t)slot * ADMIN_WQE_STRIDE;

            /* Single full-stride read (64 bytes covers all current admin ops).
             * The header is [0:3] and the body occupies [4:63].
             * Ops with body > 60 bytes span 2 strides; read the second stride
             * only when needed to avoid an extra DMA round-trip. */
            uint8_t wqe_buf[4 * ADMIN_WQE_STRIDE]; /* 256 bytes, no VLA */
            if (dma_read(vfu_ctx, wqe_gpa, wqe_buf, ADMIN_WQE_STRIDE) < 0)
                break;

            uint8_t op = wqe_buf[0];
            uint16_t len;
            memcpy(&len, wqe_buf + 2, 2);
            len = le16toh(len);

            /* Read additional strides for large WQEs (e.g. CREATE_QP = 64 bytes
             * body + 4 header = 68 bytes = 2 strides). */
            uint32_t total_bytes = ADMIN_WQE_HDR_LEN + (uint32_t)len;
            uint32_t strides =
                (total_bytes + ADMIN_WQE_STRIDE - 1u) / ADMIN_WQE_STRIDE;
            if (strides > 4)
                strides = 4;
            /* Never step past the producer index, even if len is garbage. */
            if (strides > pending - processed)
                strides = pending - processed;
            if (strides > 1) {
                uint64_t extra_gpa = wqe_gpa + ADMIN_WQE_STRIDE;
                if (dma_read(vfu_ctx, extra_gpa, wqe_buf + ADMIN_WQE_STRIDE,
                             (strides - 1u) * ADMIN_WQE_STRIDE) < 0)
                    break;
            }

            uint16_t cmd_idx = (uint16_t)slot;
            uint8_t status =
                dispatch_wqe(ctx, op, wqe_buf + ADMIN_WQE_HDR_LEN, len);
            post_admin_cqe(ctx, r, cmd_idx, op, status);
            pvrdma_adminq_count(ctx->pvrdma_handle);

            /* Never write back to guest WQE memory — the driver owns the ring.
             * Use producer-index tracking to avoid re-processing stale entries.
             */
            /* The driver consumes one stride per WQE stride, so a multi-stride
             * WQE advances its consumer index by more than one. */
            r->aq_cons = (r->aq_cons + strides) % r->aq_depth;
            processed += strides;
        }
    }
}
