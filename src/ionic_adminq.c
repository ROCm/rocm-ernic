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
#include <sys/mman.h>  /* PROT_READ, PROT_WRITE */

#include <vfio-user/libvfio-user.h>

#include "ionic_adminq.h"
#include "rocm_ernic_compat.h"

/* -------------------------------------------------------------------------
 * ionic_fw.h constants (keep in sync with pinned kernel ref)
 * -------------------------------------------------------------------------
 */

/* Admin WQE stride and header length */
#define ADMIN_WQE_STRIDE  64
#define ADMIN_WQE_HDR_LEN  4

/* Admin opcodes (enum ionic_v1_admin_op) */
enum ionic_v1_admin_op {
    IONIC_V1_ADMIN_NOOP           = 0,
    IONIC_V1_ADMIN_CREATE_CQ      = 1,
    IONIC_V1_ADMIN_CREATE_QP      = 2,
    IONIC_V1_ADMIN_CREATE_MR      = 3,
    IONIC_V1_ADMIN_STATS_HDRS     = 4,
    IONIC_V1_ADMIN_STATS_VALS     = 5,
    IONIC_V1_ADMIN_DESTROY_MR     = 6,
    /* 7 reserved */
    IONIC_V1_ADMIN_DESTROY_CQ     = 8,
    IONIC_V1_ADMIN_MODIFY_QP      = 9,
    IONIC_V1_ADMIN_QUERY_QP       = 10,
    IONIC_V1_ADMIN_DESTROY_QP     = 11,
    IONIC_V1_ADMIN_DEBUG          = 12,
    IONIC_V1_ADMIN_CREATE_AH      = 13,
    IONIC_V1_ADMIN_QUERY_AH       = 14,
    IONIC_V1_ADMIN_MODIFY_DCQCN   = 15,
    IONIC_V1_ADMIN_DESTROY_AH     = 16,
    IONIC_V1_ADMIN_QP_STATS_HDRS  = 17,
    IONIC_V1_ADMIN_QP_STATS_VALS  = 18,
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
#define CQE_COLOR_BIT   0x01u
#define CQE_ERROR_BIT   0x02u
/* Type field bits[7:5] = 0 means admin CQE; we always write 0 there. */

/* -------------------------------------------------------------------------
 * Per-AQ ring state
 * -------------------------------------------------------------------------
 */
#define MAX_AQ 4

struct ionic_aq_ring {
    bool     valid;

    /* AQ WQE ring */
    uint64_t aq_dma;           /* guest PA of WQE ring */
    uint32_t aq_depth;         /* number of entries (2^depth_log2) */
    uint32_t aq_cons;          /* our consumer index (next WQE to read) */
    uint32_t aq_prod;          /* last producer index from doorbell write */

    /* Admin CQ ring */
    uint64_t cq_dma;           /* guest PA of CQE ring */
    uint32_t cq_depth;         /* number of entries */
    uint32_t cq_prod;          /* our producer index */
    bool     cq_color;         /* current color (flips on ring wrap) */
};

struct ionic_adminq_ctx {
    vfu_ctx_t            *vfu_ctx;
    struct ionic_aq_ring  aq[MAX_AQ];
    int                   aq_count;

    /* pvrdma handle for calling ionic_rm_* compat wrappers. */
    pvrdma_handle_t       pvrdma_handle;
};

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

void ionic_adminq_register_queue(struct ionic_adminq_ctx *ctx,
                                  int aq_idx,
                                  uint64_t aq_dma, uint8_t aq_depth_log2,
                                  uint64_t cq_dma, uint8_t cq_depth_log2)
{
    if (!ctx || aq_idx < 0 || aq_idx >= MAX_AQ)
        return;

    struct ionic_aq_ring *r = &ctx->aq[aq_idx];
    r->valid     = true;
    r->aq_dma    = aq_dma;
    r->aq_depth  = 1u << aq_depth_log2;
    r->aq_cons   = 0;
    r->aq_prod   = 0;
    r->cq_dma    = cq_dma;
    r->cq_depth  = 1u << cq_depth_log2;
    r->cq_prod   = 0;
    r->cq_color  = true;  /* ionic: initial color = true */

    if (aq_idx >= ctx->aq_count)
        ctx->aq_count = aq_idx + 1;

    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq: registered AQ[%d] aq_dma=%#lx depth=%u "
            "cq_dma=%#lx cq_depth=%u",
            aq_idx, aq_dma, r->aq_depth, cq_dma, r->cq_depth);
}

void ionic_adminq_set_resources(struct ionic_adminq_ctx *ctx,
                                 void *dev_res, void *backend_dev)
{
    /* dev_res and backend_dev are not stored directly — we use the
     * ionic_rm_* compat wrappers via pvrdma_handle instead. */
    (void)dev_res; (void)backend_dev;
    /* pvrdma_handle must be set separately via ionic_adminq_set_pvrdma. */
}

void ionic_adminq_set_pvrdma(struct ionic_adminq_ctx *ctx, void *handle)
{
    if (ctx)
        ctx->pvrdma_handle = (pvrdma_handle_t)handle;
}

void ionic_adminq_update_prod(struct ionic_adminq_ctx *ctx,
                               int aq_idx, uint16_t p_index)
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

    ret = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)(uintptr_t)gpa, len,
                          sg, 1, PROT_READ);
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

static int dma_write(vfu_ctx_t *vfu_ctx, uint64_t gpa,
                     const void *buf, size_t len)
{
    dma_sg_t *sg = malloc(dma_sg_size());
    struct iovec iov;
    int ret;

    if (!sg)
        return -ENOMEM;

    ret = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)(uintptr_t)gpa, len,
                          sg, 1, PROT_WRITE);
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
 * Post an admin CQE
 * -------------------------------------------------------------------------
 */

static void post_admin_cqe(struct ionic_adminq_ctx *ctx,
                            struct ionic_aq_ring *r,
                            uint16_t cmd_idx, uint8_t cmd_op,
                            uint8_t status)
{
    /* ionic_v1_cqe layout for admin completions (32 bytes, see ionic_fw.h):
     *   [0:1]  le16 cmd_idx   — AQ consumer index this completes
     *   [2]    u8  cmd_op
     *   [3:19] u8  rsvd[17]
     *   [20:23] be32 status_length  (status in bits[31:24] when error set)
     *   [24:27] be32 qid_type_flags (color[0], error[1], type[7:5], qid[31:8])
     *   [28:31] padding (zeroed)
     */
    uint8_t cqe[CQE_SIZE];
    memset(cqe, 0, CQE_SIZE);

    uint16_t ci = htole16(cmd_idx);
    memcpy(cqe + 0, &ci, 2);    /* cmd_idx le16  */
    cqe[2] = cmd_op;

    /* status_length at byte 20 (be32) */
    uint32_t sl = htobe32(status ? ((uint32_t)status << 24) : 0);
    memcpy(cqe + 20, &sl, 4);

    /* qid_type_flags at byte 24 (be32):
     *   bit 0 = color, bit 1 = error, bits[7:5] = type (0=admin), qid=0 */
    uint32_t qtf = (uint32_t)(r->cq_color ? CQE_COLOR_BIT : 0) |
                   (status ? CQE_ERROR_BIT : 0);
    qtf = htobe32(qtf);
    memcpy(cqe + 24, &qtf, 4);

    /* Write CQE to guest memory */
    uint64_t cqe_gpa = r->cq_dma +
                       (uint64_t)(r->cq_prod % r->cq_depth) * CQE_SIZE;
    if (dma_write(ctx->vfu_ctx, cqe_gpa, cqe, CQE_SIZE) < 0) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq: failed to write CQE to %#lx", cqe_gpa);
        return;
    }

    r->cq_prod++;
    if (r->cq_prod % r->cq_depth == 0)
        r->cq_color = !r->cq_color;  /* color flips on ring wrap */
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
                                 1u << body[4],  /* cqe count = 2^depth */
                                 &cq_handle);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_CQ %u: rdma_rm_alloc_cq failed (%d)",
                cq_id, ret);
        return 1;
    }

    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq CREATE_CQ cq_id=%u handle=%u depth=2^%u",
            cq_id, cq_handle, body[4]);
    return 0;
}

/* -------------------------------------------------------------------------
 * ionic_admin_create_qp body (64 bytes from ionic_fw.h):
 *   le32 pd_id        [0:3]
 *   le32 priv_flags   [4:7]
 *   le32 sq_cq_id     [8:11]
 *   u8   sq_depth     [12]
 *   u8   sq_stride    [13]
 *   ...
 *   le32 id_ver       [52:55]  (qpid | ver<<24)
 *   le16 dbid_flags   [56:57]
 *   u8   type_state   [58]     qp_type | (state<<4)
 * -------------------------------------------------------------------------
 */
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

    uint32_t pd_id;
    memcpy(&pd_id, body + 0, 4);
    pd_id = le32toh(pd_id);

    uint32_t sq_cq_id;
    memcpy(&sq_cq_id, body + 8, 4);
    sq_cq_id = le32toh(sq_cq_id);

    uint32_t id_ver;
    memcpy(&id_ver, body + 52, 4);
    id_ver = le32toh(id_ver);
    uint32_t qp_id = id_ver & 0x00ffffffu;

    uint8_t type_state = body[58];
    uint8_t qp_type    = type_state & 0x0fu;
    uint8_t qp_state   = (type_state >> 4) & 0x0fu;
    (void)qp_state;

    uint32_t qpn;
    int ret = ionic_rm_alloc_qp(ctx->pvrdma_handle, pd_id,
                                 qp_type,
                                 (uint32_t)(1u << body[12]),  /* max_send_wr */
                                 (uint32_t)(1u << body[16]),  /* max_recv_wr */
                                 sq_cq_id,                    /* send_cq_handle */
                                 sq_cq_id,                    /* recv_cq_handle */
                                 &qpn);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP %u: rdma_rm_alloc_qp failed (%d)",
                qp_id, ret);
        return 1;
    }

    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq CREATE_QP qp_id=%u qpn=%u type=%u pd=%u",
            qp_id, qpn, qp_type, pd_id);
    return 0;
}

/* -------------------------------------------------------------------------
 * WQE dispatch
 * -------------------------------------------------------------------------
 */

static uint8_t dispatch_wqe(struct ionic_adminq_ctx *ctx,
                             uint8_t op, const uint8_t *body, uint16_t len)
{
    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq: WQE op=%u len=%u", op, len);

    switch (op) {
    case IONIC_V1_ADMIN_NOOP:
        return 0;

    case IONIC_V1_ADMIN_CREATE_CQ:
        return handle_create_cq_op(ctx, body, len);

    case IONIC_V1_ADMIN_CREATE_QP:
        return handle_create_qp_op(ctx, body, len);

    case IONIC_V1_ADMIN_CREATE_MR: {
        if (len < 45 || !ctx->pvrdma_handle) { return 0; }
        uint32_t pd_id, id_ver;
        memcpy(&pd_id,  body + 8,  4);  pd_id  = le32toh(pd_id);
        memcpy(&id_ver, body + 16, 4);  id_ver = le32toh(id_ver);
        uint16_t flags;
        memcpy(&flags,  body + 40, 2);  flags  = le16toh(flags);
        uint32_t mr_handle;
        int ret = ionic_rm_alloc_mr(ctx->pvrdma_handle, pd_id,
                                     (uint32_t)flags, &mr_handle);
        if (ret) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq CREATE_MR: failed (%d)", ret);
            return 1;
        }
        vfu_log(ctx->vfu_ctx, LOG_INFO,
                "ionic_adminq CREATE_MR mr_id=%u handle=%u",
                id_ver & 0xffffffu, mr_handle);
        return 0;
    }

    case IONIC_V1_ADMIN_DESTROY_MR: {
        if (len < 4 || !ctx->pvrdma_handle) { return 0; }
        uint32_t mr_handle;
        memcpy(&mr_handle, body, 4);  mr_handle = le32toh(mr_handle);
        ionic_rm_dealloc_mr(ctx->pvrdma_handle, mr_handle);
        return 0;
    }

    case IONIC_V1_ADMIN_DESTROY_CQ: {
        if (len < 4 || !ctx->pvrdma_handle) { return 0; }
        uint32_t cq_handle;
        memcpy(&cq_handle, body, 4);  cq_handle = le32toh(cq_handle);
        ionic_rm_dealloc_cq(ctx->pvrdma_handle, cq_handle);
        return 0;
    }

    case IONIC_V1_ADMIN_DESTROY_QP: {
        if (len < 4 || !ctx->pvrdma_handle) { return 0; }
        uint32_t qpn;
        memcpy(&qpn, body, 4);  qpn = le32toh(qpn);
        ionic_rm_dealloc_qp(ctx->pvrdma_handle, qpn);
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
        if (len < 60 || !ctx->pvrdma_handle) { return 0; }

        uint32_t attr_mask_be;
        memcpy(&attr_mask_be, body + 0, 4);
        uint32_t attr_mask = be32toh(attr_mask_be);

        uint32_t rq_psn, sq_psn, qkey_dest_qpn, id_ver;
        memcpy(&rq_psn,        body + 8,  4);  rq_psn        = le32toh(rq_psn);
        memcpy(&sq_psn,        body + 12, 4);  sq_psn        = le32toh(sq_psn);
        memcpy(&qkey_dest_qpn, body + 16, 4);  qkey_dest_qpn = le32toh(qkey_dest_qpn);
        memcpy(&id_ver,        body + 56, 4);  id_ver        = le32toh(id_ver);

        uint8_t  type_state = body[39];
        uint32_t qpn        = id_ver & 0x00ffffffu;

        /* AH DMA address carries the destination GID in some transitions.
         * We read 16 bytes from the ah_id_len field area as a proxy for
         * dgid; a full implementation would DMA-read from dma_addr. */
        uint8_t dgid_zeros[16] = {0};

        int ret = ionic_rm_modify_qp(ctx->pvrdma_handle, qpn,
                                      attr_mask, type_state,
                                      sq_psn, rq_psn,
                                      qkey_dest_qpn,
                                      dgid_zeros);
        if (ret) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq MODIFY_QP %u: failed (%d)", qpn, ret);
            return 1;
        }
        vfu_log(ctx->vfu_ctx, LOG_INFO,
                "ionic_adminq MODIFY_QP qpn=%u type_state=%#x attr=%#x",
                qpn, type_state, attr_mask);
        return 0;
    }

    case IONIC_V1_ADMIN_QUERY_QP:
    case IONIC_V1_ADMIN_CREATE_AH:
    case IONIC_V1_ADMIN_QUERY_AH:
    case IONIC_V1_ADMIN_DESTROY_AH:
        /* Remaining opcodes: stub */
        (void)body; (void)len;
        vfu_log(ctx->vfu_ctx, LOG_WARNING,
                "ionic_adminq: op=%u stub (succeeds)", op);
        return 0;

    case IONIC_V1_ADMIN_STATS_HDRS:
    case IONIC_V1_ADMIN_STATS_VALS:
    case IONIC_V1_ADMIN_QP_STATS_HDRS:
    case IONIC_V1_ADMIN_QP_STATS_VALS:
    case IONIC_V1_ADMIN_MODIFY_DCQCN:
    case IONIC_V1_ADMIN_DEBUG:
        return 0;  /* benign stub */

    default:
        vfu_log(ctx->vfu_ctx, LOG_WARNING,
                "ionic_adminq: unknown op=%u", op);
        return 1;  /* IONIC_RC_ENOSUPP */
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

        /* Process WQEs between aq_cons and aq_prod (set by doorbell writes).
         * Using producer index is the only correct way to detect pending work:
         * - op==0 (NOOP) with len==0 is a valid WQE, not an empty slot marker.
         * - Stale zeroed slots after a ring wrap look identical to empty slots.
         * When aq_prod hasn't been updated yet (no doorbell received), we fall
         * back to scanning one stride and stopping on a zero op+len — this
         * handles the bootstrapping case before the first doorbell arrives. */
        uint32_t pending;
        if (r->aq_prod != r->aq_cons) {
            /* Doorbell-driven path: exact WQE count known */
            pending = (r->aq_prod - r->aq_cons + r->aq_depth) % r->aq_depth;
        } else {
            /* No pending doorbells: scan at most one stride speculatively */
            pending = 1;
        }

        uint32_t processed = 0;
        while (processed < pending && processed < r->aq_depth) {
            uint32_t slot    = r->aq_cons % r->aq_depth;
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

            /* Speculative scan: if no doorbell arrived and the slot is empty,
             * stop. An empty slot has op=0 AND len=0 AND all body bytes 0. */
            if (r->aq_prod == r->aq_cons && op == 0 && len == 0)
                break;

            /* Read additional strides for large WQEs (e.g. CREATE_QP = 64 bytes
             * body + 4 header = 68 bytes = 2 strides). */
            uint32_t total_bytes = ADMIN_WQE_HDR_LEN + (uint32_t)len;
            uint32_t strides = (total_bytes + ADMIN_WQE_STRIDE - 1u) /
                               ADMIN_WQE_STRIDE;
            if (strides > 4)
                strides = 4;
            if (strides > 1) {
                uint64_t extra_gpa = wqe_gpa + ADMIN_WQE_STRIDE;
                dma_read(vfu_ctx, extra_gpa, wqe_buf + ADMIN_WQE_STRIDE,
                         (strides - 1u) * ADMIN_WQE_STRIDE);
            }

            uint16_t cmd_idx = (uint16_t)slot;
            uint8_t  status  = dispatch_wqe(ctx, op,
                                            wqe_buf + ADMIN_WQE_HDR_LEN,
                                            len);
            post_admin_cqe(ctx, r, cmd_idx, op, status);

            /* Never write back to guest WQE memory — the driver owns the ring.
             * Use producer-index tracking to avoid re-processing stale entries. */
            r->aq_cons = (r->aq_cons + 1u) % r->aq_depth;
            processed++;
        }
    }
}
