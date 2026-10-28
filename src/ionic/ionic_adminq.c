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
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>

#include <infiniband/verbs.h>
#include <vfio-user/libvfio-user.h>

#include "byteorder.h"
#include "ernic_dma.h"
#include "ernic_ib.h"
#include "ernic_limits.h"
#include "ernic_rm.h"
#include "ernic_stats.h"
#include "ionic/ionic_adminq.h"
#include "ionic/ionic_adminq_priv.h"
#include "ionic/ionic_datapath.h"
#include "ionic/ionic_fault.h"
#include "ionic/ionic_status.h"

/* -------------------------------------------------------------------------
 * ionic_fw.h constants (keep in sync with pinned kernel ref)
 * -------------------------------------------------------------------------
 */

/* Admin WQE stride and header length */
#define ADMIN_WQE_STRIDE  64
#define ADMIN_WQE_HDR_LEN 4

/* The most strides a WQE may span. The longest body the driver sends,
 * CREATE_QP's, takes two. */
#define ADMIN_WQE_MAX_STRIDES 4u

/* A completion names its WQE's slot in a 16-bit cmd_idx. */
_Static_assert(IONIC_ADMINQ_MAX_DEPTH_LOG2 <= 16,
               "an admin queue slot must fit a completion's cmd_idx");

/*
 * ionic_v1_admin_wqe header (4 bytes):
 *   u8  op
 *   u8  rsvd
 *   le16 len   (total byte length of cmd body)
 *
 * Followed by the opcode-specific command body.  A WQE may span multiple
 * 64-byte strides if len+4 > 64.  The driver produces each stride as its own
 * ring entry (ionic_admin_poll_locked()), so a WQE that reaches the end of
 * the ring continues at its start.
 *
 * ionic_v1_cqe (32 bytes, big-endian):
 *   For admin CQE type (IONIC_V1_CQE_TYPE_ADMIN = 0):
 *     le16 cmd_idx     — AQ consumer index this completes
 *     u8   cmd_op
 *     u8   rsvd[17]
 *     le16 old_sq_cindex
 *     le16 old_rq_cq_cindex
 *   Followed by (at byte 28):
 *     be32 status_length   (the ionic status code when error bit set)
 *     be32 qid_type_flags
 *       bit 0:  color
 *       bit 1:  error
 *       bits[7:5]: type  (0 = admin)
 *       bits[31:8]: qid
 */
/* CQE_SIZE, CQE_COLOR_BIT and CQE_ERROR_BIT are in ionic_adminq_priv.h.
 * Type field bits[7:5] = 0 means admin CQE; we always write 0 there. */

/* enum ionic_mrf_bits: the low 12 bits are IB access flags. */
#define IONIC_MRF_ACCESS_MASK 0x0fffu

void ionic_adminq_set_datapath(struct ionic_adminq_ctx *ctx,
                               struct ionic_datapath *dp)
{
    if (ctx)
        ctx->dp = dp;
}

void ionic_adminq_set_faults(struct ionic_adminq_ctx *ctx,
                             struct ionic_fault_set *faults)
{
    if (ctx)
        ctx->faults = faults;
}

static int adminq_get_pd(struct ionic_adminq_ctx *ctx, uint32_t *pd_handle)
{
    if (!ctx->pd_valid) {
        int ret = ernic_rm_alloc_pd(ctx->pvrdma_handle, &ctx->pd_handle);
        if (ret)
            return ret;
        ctx->pd_valid = true;
        vfu_log(ctx->vfu_ctx, LOG_INFO, "ionic_adminq: shared PD handle=%u",
                ctx->pd_handle);
    }
    *pd_handle = ctx->pd_handle;
    return 0;
}

/*
 * A free entry in the CQ map, or NULL if it is full. CREATE_CQ takes it
 * before allocating anything, so a CQ the map can't hold is refused with
 * nothing to give back: a CQ that isn't mapped can't be found again by
 * DESTROY_CQ, so it would leak. The same goes for the QP and MR maps.
 */
static struct ionic_cq_map_entry *adminq_cq_free_entry(
    struct ionic_adminq_ctx *ctx)
{
    for (uint32_t i = 0; i < MAX_CQ_MAP; i++)
        if (!ctx->cq_map[i].valid)
            return &ctx->cq_map[i];
    return NULL;
}

/*
 * Allocate an SRQ id and its map entry together, and derive the id from the
 * slot that holds it.
 *
 * The id has to land inside the datapath's SRQ table, which is indexed by
 * srq_id - IONIC_SRQ_QID_BASE and holds ERNIC_MAX_SRQ entries. Taking
 * the id from the slot index makes that true by construction: every id this
 * can return is in range, and an id is reusable the moment DESTROY_SRQ frees
 * its slot.
 *
 * The counter this replaced was monotonic -- ++next_srq_id, decremented only
 * when the map was full and never on destroy -- so it drifted upwards with
 * churn rather than with live SRQs. After 255 cumulative creates it handed
 * out IONIC_SRQ_QID_BASE + 256, one past the table; srq_by_qid() returned
 * NULL, registration silently did nothing, and CREATE_SRQ still reported
 * success. It also started at +1, so slot 0 was never used at all.
 *
 * False when the map is full, and the caller must fail the command on it. A
 * warning is not enough: CREATE_SRQ would still return the id it allocated,
 * so the guest would hold an SRQ that every later lookup misses -- posts go
 * nowhere and the failure surfaces as a hang in the datapath rather than as
 * an error from the verb that caused it.
 */
static bool adminq_alloc_srq(struct ionic_adminq_ctx *ctx, uint32_t handle,
                             uint32_t srq_limit, uint32_t *srq_id)
{
    for (uint32_t i = 0; i < MAX_SRQ_MAP; i++) {
        if (ctx->srq_map[i].valid)
            continue;
        *srq_id = IONIC_SRQ_QID_BASE + i;
        ctx->srq_map[i] = (struct ionic_srq_map_entry){.valid = true,
                                                       .srq_id = *srq_id,
                                                       .handle = handle,
                                                       .srq_limit = srq_limit};
        return true;
    }
    vfu_log(ctx->vfu_ctx, LOG_WARNING,
            "ionic_adminq: SRQ map full at %u SRQs, refusing",
            (unsigned)MAX_SRQ_MAP);
    return false;
}

static struct ionic_srq_map_entry *adminq_srq_entry(
    struct ionic_adminq_ctx *ctx, uint32_t srq_id)
{
    for (uint32_t i = 0; i < MAX_SRQ_MAP; i++) {
        if (ctx->srq_map[i].valid && ctx->srq_map[i].srq_id == srq_id)
            return &ctx->srq_map[i];
    }
    return NULL;
}

static bool adminq_lookup_srq(struct ionic_adminq_ctx *ctx, uint32_t srq_id,
                              uint32_t *handle)
{
    const struct ionic_srq_map_entry *e = adminq_srq_entry(ctx, srq_id);

    if (!e)
        return false;
    *handle = e->handle;
    return true;
}

static void adminq_unmap_srq(struct ionic_adminq_ctx *ctx, uint32_t srq_id)
{
    for (uint32_t i = 0; i < MAX_SRQ_MAP; i++) {
        if (ctx->srq_map[i].valid && ctx->srq_map[i].srq_id == srq_id) {
            ctx->srq_map[i].valid = false;
            return;
        }
    }
}

static bool adminq_lookup_cq(const struct ionic_adminq_ctx *ctx, uint32_t cq_id,
                             uint32_t *handle)
{
    for (uint32_t i = 0; i < MAX_CQ_MAP; i++) {
        if (ctx->cq_map[i].valid && ctx->cq_map[i].cq_id == cq_id) {
            *handle = ctx->cq_map[i].handle;
            return true;
        }
    }
    return false;
}

/* A free entry in the QP map, or NULL if it is full; see
 * adminq_cq_free_entry(). */
static struct ionic_qp_map_entry *adminq_qp_free_entry(
    struct ionic_adminq_ctx *ctx)
{
    for (uint32_t i = 0; i < MAX_QP_MAP; i++)
        if (!ctx->qp_map[i].valid)
            return &ctx->qp_map[i];
    return NULL;
}

static struct ionic_qp_map_entry *adminq_qp_entry(struct ionic_adminq_ctx *ctx,
                                                  uint32_t qp_id)
{
    for (uint32_t i = 0; i < MAX_QP_MAP; i++)
        if (ctx->qp_map[i].valid && ctx->qp_map[i].qp_id == qp_id)
            return &ctx->qp_map[i];
    return NULL;
}

static bool adminq_lookup_qp(struct ionic_adminq_ctx *ctx, uint32_t qp_id,
                             uint32_t *qpn)
{
    const struct ionic_qp_map_entry *e = adminq_qp_entry(ctx, qp_id);

    if (!e)
        return false;
    *qpn = e->qpn;
    return true;
}

/* The reverse: the qpid of the QP the backend numbers @qpn. */
static bool adminq_lookup_qp_id(const struct ionic_adminq_ctx *ctx,
                                uint32_t qpn, uint32_t *qp_id)
{
    for (uint32_t i = 0; i < MAX_QP_MAP; i++) {
        if (ctx->qp_map[i].valid && ctx->qp_map[i].qpn == qpn) {
            *qp_id = ctx->qp_map[i].qp_id;
            return true;
        }
    }
    return false;
}

static void adminq_unmap_qp(struct ionic_adminq_ctx *ctx, uint32_t qp_id)
{
    for (uint32_t i = 0; i < MAX_QP_MAP; i++)
        if (ctx->qp_map[i].valid && ctx->qp_map[i].qp_id == qp_id)
            ctx->qp_map[i].valid = false;
}

/* A free entry in the MR map, or NULL if it is full; see
 * adminq_cq_free_entry(). */
static struct ionic_mr_map_entry *adminq_mr_free_entry(
    struct ionic_adminq_ctx *ctx)
{
    for (uint32_t i = 0; i < MAX_MR_MAP; i++)
        if (!ctx->mr_map[i].valid)
            return &ctx->mr_map[i];
    return NULL;
}

static bool adminq_lookup_mr(const struct ionic_adminq_ctx *ctx, uint32_t mr_id,
                             uint32_t *handle)
{
    for (uint32_t i = 0; i < MAX_MR_MAP; i++) {
        if (ctx->mr_map[i].valid && ctx->mr_map[i].mr_id == mr_id) {
            *handle = ctx->mr_map[i].handle;
            return true;
        }
    }
    return false;
}

static void adminq_unmap_mr(struct ionic_adminq_ctx *ctx, uint32_t mr_id)
{
    for (uint32_t i = 0; i < MAX_MR_MAP; i++)
        if (ctx->mr_map[i].valid && ctx->mr_map[i].mr_id == mr_id)
            ctx->mr_map[i].valid = false;
}

static void adminq_unmap_cq(struct ionic_adminq_ctx *ctx, uint32_t cq_id)
{
    for (uint32_t i = 0; i < MAX_CQ_MAP; i++)
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
    ctx->dma = ernic_dma_create();
    if (!ctx->dma) {
        free(ctx);
        return NULL;
    }
    ctx->vfu_ctx = vfu_ctx;
    return ctx;
}

void ionic_adminq_destroy(struct ionic_adminq_ctx *ctx)
{
    if (!ctx)
        return;
    ernic_dma_destroy(ctx->dma);
    free(ctx);
}

/*
 * Everything the maps hold goes, each the way its DESTROY command frees it,
 * datapath first. The maps are the only record of the resource manager's
 * handles, so without this every reset leaked what the guest had created.
 */
void ionic_adminq_release_all(struct ionic_adminq_ctx *ctx)
{
    if (!ctx)
        return;
    ionic_datapath_reset_rdma(ctx->dp);

    for (uint32_t i = 0; i < MAX_QP_MAP; i++) {
        struct ionic_qp_map_entry *e = &ctx->qp_map[i];

        if (!e->valid)
            continue;
        ernic_stats_qp_forget(ctx->stats, e->qp_id);
        ernic_rm_dealloc_qp(ctx->pvrdma_handle, e->qpn);
        e->valid = false;
    }
    for (uint32_t i = 0; i < MAX_SRQ_MAP; i++) {
        if (ctx->srq_map[i].valid) {
            ernic_rm_dealloc_srq(ctx->pvrdma_handle, ctx->srq_map[i].handle);
            ctx->srq_map[i].valid = false;
        }
    }
    for (uint32_t i = 0; i < MAX_MR_MAP; i++) {
        if (ctx->mr_map[i].valid) {
            ernic_rm_dealloc_mr(ctx->pvrdma_handle, ctx->mr_map[i].handle);
            ctx->mr_map[i].valid = false;
        }
    }
    for (uint32_t i = 0; i < MAX_CQ_MAP; i++) {
        if (ctx->cq_map[i].valid) {
            ernic_rm_dealloc_cq(ctx->pvrdma_handle, ctx->cq_map[i].handle);
            ctx->cq_map[i].valid = false;
        }
    }
    /* Last: the QPs, MRs and SRQs above were made in it */
    if (ctx->pd_valid) {
        ernic_rm_dealloc_pd(ctx->pvrdma_handle, ctx->pd_handle);
        ctx->pd_valid = false;
    }
}

bool ionic_adminq_register_queue(struct ionic_adminq_ctx *ctx, int aq_idx,
                                 uint64_t aq_dma, uint8_t aq_depth_log2,
                                 uint64_t cq_dma, uint8_t cq_depth_log2,
                                 uint32_t cq_id, uint32_t eq_id)
{
    if (!ctx || aq_idx < 0 || aq_idx >= MAX_AQ)
        return false;
    /* Guest exponents, shifted below. The strides aren't used: admin WQEs
     * and CQEs have fixed sizes. */
    if (aq_depth_log2 > IONIC_ADMINQ_MAX_DEPTH_LOG2 ||
        cq_depth_log2 > IONIC_RING_MAX_DEPTH_LOG2) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq: refusing AQ[%d] depth=2^%u cq_depth=2^%u "
                "(max 2^%u and 2^%u)",
                aq_idx, aq_depth_log2, cq_depth_log2,
                IONIC_ADMINQ_MAX_DEPTH_LOG2, IONIC_RING_MAX_DEPTH_LOG2);
        return false;
    }

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
            "ionic_adminq: registered AQ[%d] aq_dma=%#" PRIx64 " depth=%u "
            "cq_dma=%#" PRIx64 " cq_depth=%u cq_id=%u eq_id=%u",
            aq_idx, aq_dma, r->aq_depth, cq_dma, r->cq_depth, cq_id, eq_id);
    return true;
}

void ionic_adminq_set_cq_event_cb(struct ionic_adminq_ctx *ctx,
                                  ionic_adminq_cq_event_fn_t fn, void *opaque)
{
    if (ctx) {
        ctx->cq_event_fn = fn;
        ctx->cq_event_opaque = opaque;
    }
}

void ionic_adminq_set_pvrdma(struct ionic_adminq_ctx *ctx,
                             pvrdma_handle_t handle)
{
    if (ctx)
        ctx->pvrdma_handle = handle;
}

void ionic_adminq_set_stats(struct ionic_adminq_ctx *ctx,
                            struct ernic_stats *stats)
{
    if (ctx)
        ctx->stats = stats;
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
 * form the rest of the stack expects), @sgid with the source GID from the
 * same header, and @dmac with the destination MAC.
 *
 * The source is worth taking: it is the guest's own GID for the sgid_index
 * the driver resolved, and it is the only place this emulator can learn it.
 * A UD datagram leaving for another instance has to carry it in the GRH, and
 * the alternative -- reusing the destination -- tells the receiver that it
 * sent the datagram to itself.
 *
 * Returns 1 on success, 0 if the template could not be parsed.
 */
static int adminq_parse_roce_hdr(const struct ionic_adminq_ctx *ctx,
                                 uint64_t gpa, uint32_t len,
                                 uint8_t dgid[ERNIC_GID_LEN],
                                 uint8_t sgid[ERNIC_GID_LEN], uint8_t dmac[6])
{
    uint8_t hdr[128];

    if (len < 14 || len > sizeof(hdr))
        return 0;
    if (ernic_dma_read(ctx->dma, ctx->vfu_ctx, gpa, hdr, len) < 0)
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

    if (ethertype == 0x0800) { /* IPv4: saddr at +12, daddr at +16 */
        if (off + 20 > len)
            return 0;
        memset(dgid, 0, ERNIC_GID_LEN);
        dgid[10] = 0xff;
        dgid[11] = 0xff;
        memcpy(dgid + 12, hdr + off + 16, 4);
        memset(sgid, 0, ERNIC_GID_LEN);
        sgid[10] = 0xff;
        sgid[11] = 0xff;
        memcpy(sgid + 12, hdr + off + 12, 4);
        return 1;
    }

    if (ethertype == 0x86dd) { /* IPv6: saddr at +8, daddr at +24 */
        if (off + 40 > len)
            return 0;
        memcpy(dgid, hdr + off + 24, ERNIC_GID_LEN);
        memcpy(sgid, hdr + off + 8, ERNIC_GID_LEN);
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

void ionic_adminq_post_cqe(const struct ionic_adminq_ctx *ctx,
                           struct ionic_aq_ring *r, uint16_t cmd_idx,
                           uint8_t cmd_op, uint8_t status, uint32_t resp_len)
{
    /* ionic_v1_cqe layout for admin completions (32 bytes, see ionic_fw.h):
     *   [0:1]   be16 cmd_idx  — AQ consumer index this completes
     *   [2]     u8   cmd_op
     *   [3:19]  u8   rsvd[17]
     *   [20:21] le16 old_sq_cindex
     *   [22:23] le16 old_rq_cq_cindex
     *   [24:27] be32 status_length  (the status code when error is set; the
     *           driver logs be32_to_cpu() of it as "opcode %u error %u")
     *   [28:31] be32 qid_type_flags (color[0], error[1], type[7:5], qid[31:8])
     */
    uint8_t cqe[CQE_SIZE];
    memset(cqe, 0, CQE_SIZE);

    ernic_put_be16(cqe + 0, cmd_idx);
    cqe[2] = cmd_op;

    /* status_length is one field doing two jobs: the status code itself when
     * the error bit is set, and otherwise the number of response bytes
     * written. Returning 0 on success is not "no length given" to a driver
     * that reads it -- ionic_create_srq_cmd() compares it against
     * IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN and reports EOPNOTSUPP when it is
     * short, so a successful CREATE_SRQ surfaced in the guest as
     * ibv_create_srq() returning "Operation not supported" on a device that
     * had just handled the command. The ops that DMA no response still send
     * 0, which is their correct length. */
    ernic_put_be32(cqe + 24, status ? (uint32_t)status : resp_len);

    /* qid_type_flags: bit 0 = color, bit 1 = error, bits[7:5] = type
     * (0 = admin), bits[31:8] = the AQ id, which the driver checks against
     * aq->aqid before accepting the completion. */
    uint32_t qtf = (r->aq_id << 8) |
                   (uint32_t)(r->cq_color ? CQE_COLOR_BIT : 0) |
                   (status ? CQE_ERROR_BIT : 0);
    ernic_put_be32(cqe + 28, qtf);

    /* Write CQE to guest memory */
    uint64_t cqe_gpa =
        r->cq_dma + (uint64_t)(r->cq_prod % r->cq_depth) * CQE_SIZE;
    if (ernic_dma_write(ctx->dma, ctx->vfu_ctx, cqe_gpa, cqe, CQE_SIZE) < 0) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq: failed to write CQE to %#" PRIx64, cqe_gpa);
        return;
    }

    vfu_log(ctx->vfu_ctx, LOG_DEBUG,
            "ionic_adminq: CQE aq_id=%u cq_id=%u eq_id=%u cmd_idx=%u op=%u "
            "prod=%u color=%u gpa=%#" PRIx64,
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

/*
 * The status that fails a command whose data path registration returned
 * @err. The driver turns each back into the errno here
 * (ionic_error_to_errno()), so the guest learns why.
 */
static uint8_t adminq_dp_status(int err)
{
    switch (err) {
    case -EINVAL:
        return IONIC_RC_EINVAL;
    case -ENOMEM:
        return IONIC_RC_ENOMEM;
    case -ENOSPC:
        return IONIC_RC_ENOSPC;
    default:
        return IONIC_RC_EIO;
    }
}

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
        return IONIC_RC_SUCCESS;
    }
    if (len < 34) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_CQ: short body (%u)", len);
        return IONIC_RC_EINVAL;
    }

    uint32_t id_ver = ernic_get_le32(body + 12);
    uint32_t cq_id = id_ver & 0x00ffffffu;

    /*
     * The driver takes its CQ ids from below the count in the LIF identity,
     * which is all the data path's table holds. A CQ past it has no slot
     * there, so it is refused before anything is allocated for it.
     */
    if (cq_id >= ERNIC_MAX_CQ) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_CQ %u: cq_id is not below %u", cq_id,
                ERNIC_MAX_CQ);
        return IONIC_RC_EQID;
    }

    /* ionic_admin_create_cq: eq_id[0:3], depth_log2[4], stride_log2[5],
     * dir_size_log2[6], page_size_log2[7], cq_flags[8:11], id_ver[12:15],
     * tbl_index[16:19], map_count[20:23], dma_addr[24:31], dbid_flags[32:33] */
    uint32_t eq_id = ernic_get_le32(body + 0);

    struct ionic_dp_ring_desc ring = {
        .buf = {.dma_addr = ernic_get_le64(body + 24),
                .map_count = ernic_get_le32(body + 20),
                .page_size_log2 = body[7]},
        .depth_log2 = body[4],
        .stride_log2 = body[5],
    };

    /*
     * The depth is a guest exponent, and the CQ is sized by shifting by it,
     * so it is checked first, along with the stride the data path would
     * otherwise refuse only after the CQ had been allocated.
     */
    if (!ionic_dp_ring_geometry_ok(
            ring.depth_log2, ionic_dp_cq_stride_log2(&ring),
            IONIC_CQ_MIN_STRIDE_LOG2, IONIC_RING_MAX_STRIDE_LOG2)) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_CQ %u: refusing depth_log2=%u "
                "stride_log2=%u (depth max %u, stride %u..%u)",
                cq_id, ring.depth_log2, ring.stride_log2,
                IONIC_RING_MAX_DEPTH_LOG2, IONIC_CQ_MIN_STRIDE_LOG2,
                IONIC_RING_MAX_STRIDE_LOG2);
        return IONIC_RC_ERANGE;
    }

    /*
     * An id already in use is refused rather than mapped over: the CQ that
     * had it could no longer be found, and would leak.
     */
    uint32_t cq_handle;
    if (adminq_lookup_cq(ctx, cq_id, &cq_handle)) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_CQ %u: cq_id already in use", cq_id);
        return IONIC_RC_EEXIST;
    }
    struct ionic_cq_map_entry *entry = adminq_cq_free_entry(ctx);
    if (!entry) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_CQ %u: CQ map full (%u CQs)", cq_id,
                MAX_CQ_MAP);
        return IONIC_RC_ENOSPC;
    }

    int ret = ernic_rm_alloc_cq(ctx->pvrdma_handle, 1u << ring.depth_log2,
                                &cq_handle);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_CQ %u: rdma_rm_alloc_cq failed (%d)",
                cq_id, ret);
        return IONIC_RC_ERDMA;
    }

    /*
     * Its failure is the command's failure: a CQ the data path has no ring
     * for never completes anything. It is registered before the id is
     * mapped, so on failure only the resource manager's CQ needs releasing,
     * and the driver can use the id again.
     */
    ret = ionic_datapath_register_cq(ctx->dp, cq_id, eq_id, &ring);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_CQ %u: datapath registration failed (%d)",
                cq_id, ret);
        ernic_rm_dealloc_cq(ctx->pvrdma_handle, cq_handle);
        return adminq_dp_status(ret);
    }

    *entry = (struct ionic_cq_map_entry){
        .valid = true, .cq_id = cq_id, .handle = cq_handle};

    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq CREATE_CQ cq_id=%u handle=%u depth=2^%u", cq_id,
            cq_handle, ring.depth_log2);
    return IONIC_RC_SUCCESS;
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
/* enum ionic_qp_type is not enum ibv_qp_type, and rdma_rm speaks the latter.
 * The driver also folds GSI into UD, but reserves qpid 1 for it, which is the
 * only way to tell the two apart on the wire. */
static uint8_t ionic_qpt_to_ib(uint8_t ionic_qpt, uint32_t qp_id)
{
    if (qp_id == 1)
        return ERNIC_IB_QPT_GSI;
    switch (ionic_qpt) {
    case IONIC_QPT_RC:
        return IBV_QPT_RC;
    case IONIC_QPT_UC:
        return IBV_QPT_UC;
    case IONIC_QPT_UD:
        return IBV_QPT_UD;
    default:
        return IBV_QPT_RC;
    }
}

/*
 * Undo a partly built SRQ. Safe at any point after the id is allocated:
 * ionic_datapath_unregister_srq() does nothing for a slot that was never
 * registered, so the one helper serves every failure path and none of them
 * can forget a piece.
 */
static void adminq_srq_unwind(struct ionic_adminq_ctx *ctx, uint32_t srq_id,
                              uint32_t srq_handle)
{
    ionic_datapath_unregister_srq(ctx->dp, srq_id);
    ernic_rm_dealloc_srq(ctx->pvrdma_handle, srq_handle);
    adminq_unmap_srq(ctx, srq_id);
}

/*
 * CREATE_SRQ (opcode 26)
 *
 * struct ionic_admin_create_srq, 42 bytes:
 *   le64 dma_addr        [0:7]    page table of the SRQ ring
 *   le32 map_count       [8:11]
 *   le32 pd_id           [12:15]
 *   le32 qid             [16:19]  driver's hint; we assign the real one
 *   le16 dbid            [20:21]
 *   le16 low_wqes_limit  [22:23]  srq_limit -- the SRQ_LIMIT event watermark
 *   le16 flags           [24:25]  IONIC_SRQF_CMB
 *   u8   depth_log2      [26]
 *   u8   stride_log2     [27]
 *   u8   page_size_log2  [28]
 *   u8   udma_mask       [29]
 *   le32 resp_buf_len    [30:33]
 *   le64 resp_dma_addr   [34:41]
 *
 * The response is DMA'd rather than returned in the completion:
 *   struct ionic_admin_create_srq_resp, 5 bytes: le32 id, u8 udma_idx
 */
static uint8_t handle_create_srq_op(struct ionic_adminq_ctx *ctx,
                                    const uint8_t *body, uint16_t len)
{
    if (!ctx->pvrdma_handle) {
        vfu_log(ctx->vfu_ctx, LOG_WARNING,
                "ionic_adminq CREATE_SRQ: no pvrdma_handle (stub ok)");
        return IONIC_RC_SUCCESS;
    }
    if (len < IONIC_ADMIN_CREATE_SRQ_IN_V1_LEN) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_SRQ: short body (%u)", len);
        return IONIC_RC_EINVAL;
    }

    uint16_t low_wqes_limit = ernic_get_le16(body + 22);
    uint32_t resp_buf_len = ernic_get_le32(body + 30);
    uint64_t resp_dma_addr = ernic_get_le64(body + 34);

    /*
     * Bound the ring geometry before anything shifts by it. body[26..28] are
     * guest bytes: 1u << body[26] with body[26] >= 32 is undefined behaviour
     * in C, reachable from a single malformed WQE, and body[27] reaches the
     * receive path as the stride that decides how much of a WQE to walk.
     * Refuse the command rather than clamping -- a driver that asks for a
     * geometry the device never advertised is wrong, and silently giving it
     * a different one produces a queue neither side agrees about.
     */
    if (body[26] > IONIC_SRQ_MAX_DEPTH_LOG2 ||
        body[27] > IONIC_SRQ_MAX_STRIDE_LOG2 ||
        body[27] < IONIC_SRQ_MIN_STRIDE_LOG2) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_SRQ: refusing depth_log2=%u "
                "stride_log2=%u (depth max %u, stride %u..%u)",
                body[26], body[27], IONIC_SRQ_MAX_DEPTH_LOG2,
                IONIC_SRQ_MIN_STRIDE_LOG2, IONIC_SRQ_MAX_STRIDE_LOG2);
        return IONIC_RC_ERANGE;
    }

    uint32_t pd_handle;
    int ret = adminq_get_pd(ctx, &pd_handle);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_SRQ: rdma_rm_alloc_pd failed (%d)", ret);
        return IONIC_RC_ERDMA;
    }

    /* max_sge is the device's ERNIC_MAX_SRQ_SGE; the ring geometry gives
     * the depth. Bounded above, so the shift is defined. */
    uint32_t max_wr = 1u << body[26];
    uint32_t srq_handle;
    ret = ernic_rm_alloc_srq(ctx->pvrdma_handle, pd_handle, max_wr,
                             ERNIC_MAX_SRQ_SGE, low_wqes_limit, &srq_handle);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_SRQ: rdma_rm_alloc_srq failed (%d)", ret);
        return IONIC_RC_ERDMA;
    }

    /* Above the QP id space: there is no SRQ doorbell qtype, so the guest's
     * RQ doorbell carries either a QP id or this, and only the range tells
     * them apart. See IONIC_SRQ_QID_BASE. */
    uint32_t srq_id;
    if (!adminq_alloc_srq(ctx, srq_handle, low_wqes_limit, &srq_id)) {
        ernic_rm_dealloc_srq(ctx->pvrdma_handle, srq_handle);
        return IONIC_RC_ENOSPC;
    }

    /*
     * Give the datapath the ring, not just the resource. The guest posts
     * receives into this ring and rings a doorbell on the SRQ qtype; without
     * the registration that doorbell lands on an unknown queue and every
     * posted receive is lost, which shows up as a SEND to an SRQ-bound QP
     * hanging with nothing logged here. The id is ours, not the guest's
     * qid field: the driver adopts what CREATE_SRQ returns as srq->rq.qid
     * and uses it for both the doorbell and CREATE_QP's srq reference.
     *
     * Its failure is the command's failure. Reporting success for an SRQ
     * with no receive ring hands the guest a queue that accepts posts and
     * completes nothing -- the silent hang this whole feature exists to
     * remove, arriving by a different road.
     */
    {
        struct ionic_dp_ring_desc srq_ring = {
            .buf = {.dma_addr = ernic_get_le64(body + 0),
                    .map_count = ernic_get_le32(body + 8),
                    .page_size_log2 = body[28]},
            .depth_log2 = body[26],
            .stride_log2 = body[27],
        };
        int reg = ionic_datapath_register_srq(ctx->dp, srq_id, &srq_ring);
        if (reg) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq CREATE_SRQ %u: datapath registration failed "
                    "(%d)",
                    srq_id, reg);
            adminq_srq_unwind(ctx, srq_id, srq_handle);
            return IONIC_RC_EIO;
        }
    }

    /*
     * Hand the id back by DMA. The driver adopts what this returns as
     * srq->rq.qid and names it in every later MODIFY, QUERY and DESTROY, so
     * an SRQ whose id never reaches the guest is one the guest can neither
     * use nor free: it occupies a table slot until the device is reset, and
     * 256 of them exhaust the table.
     *
     * That makes an unusable response buffer the command's failure, not
     * something to skip past. Reporting success without writing the id left
     * the driver holding the zero its buffer came with, which names SRQ id
     * IONIC_SRQ_QID_BASE + 0 -- a real and probably someone else's queue.
     */
    if (resp_buf_len < IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN || !resp_dma_addr) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_SRQ %u: response buffer is %u bytes at "
                "%#" PRIx64 ", need %u at a non-zero address; the driver "
                "could not learn the id",
                srq_id, resp_buf_len, resp_dma_addr,
                IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN);
        adminq_srq_unwind(ctx, srq_id, srq_handle);
        return IONIC_RC_EINVAL;
    }

    {
        uint8_t resp[IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN] = {0};
        ernic_put_le32(resp, srq_id);
        resp[4] = 0; /* udma_idx: single udma in this emulation */
        if (ernic_dma_write(ctx->dma, ctx->vfu_ctx, resp_dma_addr, resp,
                            sizeof(resp)) < 0) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq CREATE_SRQ %u: response DMA failed", srq_id);
            adminq_srq_unwind(ctx, srq_id, srq_handle);
            return IONIC_RC_EIO;
        }
        /* What the driver compares against
         * IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN before trusting resp_buf. */
        ctx->resp_len = IONIC_ADMIN_CREATE_SRQ_OUT_V1_LEN;
    }

    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq CREATE_SRQ srq_id=%u handle=%u depth=2^%u limit=%u",
            srq_id, srq_handle, body[26], low_wqes_limit);
    return IONIC_RC_SUCCESS;
}

/* MODIFY_SRQ (27): le32 qid, le16 low_wqes_limit */
static uint8_t handle_modify_srq_op(struct ionic_adminq_ctx *ctx,
                                    const uint8_t *body, uint16_t len)
{
    if (!ctx->pvrdma_handle)
        return IONIC_RC_SUCCESS;
    if (len < 6) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq MODIFY_SRQ: short body (%u)", len);
        return IONIC_RC_EINVAL;
    }

    uint32_t qid = ernic_get_le32(body + 0);

    struct ionic_srq_map_entry *e = adminq_srq_entry(ctx, qid);
    if (!e) {
        vfu_log(ctx->vfu_ctx, LOG_ERR, "ionic_adminq MODIFY_SRQ: unknown id %u",
                qid);
        return IONIC_RC_ENOENT;
    }

    uint32_t limit = ernic_get_le16(body + 4);

    /*
     * Zero disables the low-watermark and is a legal value; the resource
     * manager rejects it, so it never goes there. Non-zero still does, to
     * keep its copy from drifting for anything that reads the backend
     * object directly.
     *
     * NOTE: setting the limit is all that happens. Raising
     * IBV_EVENT_SRQ_LIMIT_REACHED when the queue drains past it would need
     * an asynchronous event channel to the guest, and this emulator has
     * none -- the only path to the driver is a CQ completion through an EQ.
     * A consumer that arms the watermark and waits for the event will wait
     * forever. UCX's rc_verbs does not: it refills from its own accounting.
     */
    if (limit) {
        int ret = ernic_rm_modify_srq(ctx->pvrdma_handle, e->handle, limit);
        if (ret) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq MODIFY_SRQ %u: failed (%d)", qid, ret);
            return IONIC_RC_ERDMA;
        }
    }
    e->srq_limit = limit;

    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq MODIFY_SRQ srq_id=%u limit=%u", qid, limit);
    return IONIC_RC_SUCCESS;
}

/* QUERY_SRQ (28): le32 qid. Attributes are read back through the resource
 * manager; the driver keeps its own copy of depth and sge, so this exists to
 * confirm the queue is live rather than to transport them. */
static uint8_t handle_query_srq_op(struct ionic_adminq_ctx *ctx,
                                   const uint8_t *body, uint16_t len)
{
    if (!ctx->pvrdma_handle)
        return IONIC_RC_SUCCESS;
    if (len < 4) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq QUERY_SRQ: short body (%u)", len);
        return IONIC_RC_EINVAL;
    }

    uint32_t qid = ernic_get_le32(body + 0);

    const struct ionic_srq_map_entry *e = adminq_srq_entry(ctx, qid);
    if (!e) {
        vfu_log(ctx->vfu_ctx, LOG_ERR, "ionic_adminq QUERY_SRQ: unknown id %u",
                qid);
        return IONIC_RC_ENOENT;
    }

    uint32_t max_wr = 0, max_sge = 0, rm_limit = 0;
    if (ernic_rm_query_srq(ctx->pvrdma_handle, e->handle, &max_wr, &max_sge,
                           &rm_limit)) {
        return IONIC_RC_ERDMA;
    }

    /* The limit comes from the map, not from rm_limit: a limit of 0 never
     * reaches the resource manager, so its copy would read back as whatever
     * was set last rather than as "disabled". */
    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq QUERY_SRQ srq_id=%u max_wr=%u max_sge=%u limit=%u",
            qid, max_wr, max_sge, e->srq_limit);
    return IONIC_RC_SUCCESS;
}

/* DESTROY_SRQ (29): le32 qid */
static uint8_t handle_destroy_srq_op(struct ionic_adminq_ctx *ctx,
                                     const uint8_t *body, uint16_t len)
{
    if (!ctx->pvrdma_handle)
        return IONIC_RC_SUCCESS;
    if (len < 4) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq DESTROY_SRQ: short body (%u)", len);
        return IONIC_RC_EINVAL;
    }

    uint32_t qid = ernic_get_le32(body + 0);

    uint32_t handle;
    if (!adminq_lookup_srq(ctx, qid, &handle)) {
        /* Not an error worth failing the command for: a destroy of something
         * already gone is harmless, and the driver cannot retry it usefully. */
        vfu_log(ctx->vfu_ctx, LOG_WARNING,
                "ionic_adminq DESTROY_SRQ: unknown id %u", qid);
        return IONIC_RC_SUCCESS;
    }

    /* Drop the ring before the resource: a receive in flight on another
     * thread takes the SRQ lock, and unregister clears valid under it. */
    ionic_datapath_unregister_srq(ctx->dp, qid);

    ernic_rm_dealloc_srq(ctx->pvrdma_handle, handle);
    adminq_unmap_srq(ctx, qid);

    vfu_log(ctx->vfu_ctx, LOG_INFO, "ionic_adminq DESTROY_SRQ srq_id=%u", qid);
    return IONIC_RC_SUCCESS;
}

static uint8_t handle_create_qp_op(struct ionic_adminq_ctx *ctx,
                                   const uint8_t *body, uint16_t len)
{
    if (!ctx->pvrdma_handle) {
        vfu_log(ctx->vfu_ctx, LOG_WARNING,
                "ionic_adminq CREATE_QP: no pvrdma_handle (stub ok)");
        return IONIC_RC_SUCCESS;
    }
    if (len < IONIC_ADMIN_CREATE_QP_IN_V1_LEN) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP: short body (%u, need %u)", len,
                IONIC_ADMIN_CREATE_QP_IN_V1_LEN);
        return IONIC_RC_EINVAL;
    }

    uint32_t sq_cq_id = ernic_get_le32(body + 8);
    uint32_t rq_cq_id = ernic_get_le32(body + 32);
    uint32_t qp_id = ernic_get_le32(body + 56) & 0x00ffffffu;

    uint8_t type_state = body[62];
    uint8_t ib_qp_type = ionic_qpt_to_ib(type_state & 0x0fu, qp_id);

    /* As for a CQ: the data path holds QP ids below the identity's count. */
    if (qp_id >= ERNIC_MAX_QP) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP %u: qp_id is not below %u", qp_id,
                ERNIC_MAX_QP);
        return IONIC_RC_EQID;
    }

    /*
     * Refused before anything is allocated, like the checks below: an id
     * already in use, which mapped over would lose the QP that had it, and
     * a full map, which would leave the new one unmapped. Either QP could
     * then never be found again by DESTROY_QP, and would leak.
     */
    if (adminq_qp_entry(ctx, qp_id)) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP %u: qp_id already in use", qp_id);
        return IONIC_RC_EEXIST;
    }
    struct ionic_qp_map_entry *entry = adminq_qp_free_entry(ctx);
    if (!entry) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP %u: QP map full (%u QPs)", qp_id,
                MAX_QP_MAP);
        return IONIC_RC_ENOSPC;
    }

    uint32_t send_cq, recv_cq;
    if (!adminq_lookup_cq(ctx, sq_cq_id, &send_cq) ||
        !adminq_lookup_cq(ctx, rq_cq_id, &recv_cq)) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP %u: unknown cq (sq=%u rq=%u)", qp_id,
                sq_cq_id, rq_cq_id);
        return IONIC_RC_ENOENT;
    }

    /*
     * rq_tbl_index_srq_id is overloaded: ~0 means "no SRQ", anything else
     * is the SRQ's qid. Only an SRQ we actually created will do --
     * ionic_datapath_bind_qp_srq() checks the range but not whether
     * anything lives at it, since srq_by_qid() returns the slot either way,
     * and a QP bound to an empty slot has no receive ring at all: it
     * accepts receives and completes none of them.
     *
     * Checked here, before anything is allocated, rather than at the bind
     * below. Refusing after ernic_rm_alloc_qp(), the QP map entry and
     * ionic_datapath_register_qp() have run would leak a backend QP and
     * leave qp_id mapped on every rejected command, so a guest could
     * exhaust the table with commands the device is busy refusing.
     */
    uint32_t rq_tbl_index_srq_id = ernic_get_le32(body + 40);
    if (rq_tbl_index_srq_id != 0xffffffffu &&
        !adminq_srq_entry(ctx, rq_tbl_index_srq_id)) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP %u: names unknown SRQ %u", qp_id,
                rq_tbl_index_srq_id);
        return IONIC_RC_ENOENT;
    }

    struct ionic_dp_ring_desc sq = {
        .buf = {.dma_addr = ernic_get_le64(body + 24),
                .map_count = ernic_get_le32(body + 20),
                .page_size_log2 = body[15]},
        .depth_log2 = body[12],
        .stride_log2 = body[13],
    };
    struct ionic_dp_ring_desc rq = {
        .buf = {.dma_addr = ernic_get_le64(body + 48),
                .map_count = ernic_get_le32(body + 44),
                .page_size_log2 = body[39]},
        .depth_log2 = body[36],
        .stride_log2 = body[37],
    };

    /*
     * The ring geometry is guest exponents, and the work request counts
     * below are shifts by them, so it is checked here too, before anything
     * is allocated. A QP bound to an SRQ has no receive ring and leaves its
     * geometry zero, so its RQ is held only to the ceilings.
     */
    uint8_t rq_min_stride_log2 =
        rq_tbl_index_srq_id == 0xffffffffu ? IONIC_WQE_MIN_STRIDE_LOG2 : 0;
    if (!ionic_dp_ring_geometry_ok(sq.depth_log2, sq.stride_log2,
                                   IONIC_WQE_MIN_STRIDE_LOG2,
                                   IONIC_WQE_MAX_STRIDE_LOG2) ||
        !ionic_dp_ring_geometry_ok(rq.depth_log2, rq.stride_log2,
                                   rq_min_stride_log2,
                                   IONIC_WQE_MAX_STRIDE_LOG2)) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP %u: refusing sq(depth_log2=%u "
                "stride_log2=%u) rq(depth_log2=%u stride_log2=%u) (depth max "
                "%u, stride %u..%u)",
                qp_id, sq.depth_log2, sq.stride_log2, rq.depth_log2,
                rq.stride_log2, IONIC_RING_MAX_DEPTH_LOG2,
                IONIC_WQE_MIN_STRIDE_LOG2, IONIC_WQE_MAX_STRIDE_LOG2);
        return IONIC_RC_ERANGE;
    }

    uint32_t pd_handle;
    int ret = adminq_get_pd(ctx, &pd_handle);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP %u: rdma_rm_alloc_pd failed (%d)",
                qp_id, ret);
        return IONIC_RC_ERDMA;
    }

    uint32_t qpn;
    ret = ernic_rm_alloc_qp(ctx->pvrdma_handle, pd_handle, ib_qp_type,
                            1u << sq.depth_log2, /* max_send_wr */
                            1u << rq.depth_log2, /* max_recv_wr */
                            send_cq, recv_cq, &qpn);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP %u: rdma_rm_alloc_qp failed (%d)",
                qp_id, ret);
        return IONIC_RC_ERDMA;
    }

    /* As for CREATE_CQ: a QP with no rings ignores its doorbells. */
    ret = ionic_datapath_register_qp(ctx->dp, qp_id, ib_qp_type, sq_cq_id, &sq,
                                     rq_cq_id, &rq);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_QP %u: datapath registration failed (%d)",
                qp_id, ret);
        ernic_rm_dealloc_qp(ctx->pvrdma_handle, qpn);
        return adminq_dp_status(ret);
    }

    *entry =
        (struct ionic_qp_map_entry){.valid = true, .qp_id = qp_id, .qpn = qpn};

    /*
     * The id was read and checked before any of the above was allocated.
     * ~0 means "no SRQ"; anything else named an SRQ that exists, and the
     * driver left every rq_* field zero because the QP has no receive ring
     * of its own. Binding is what sends its receives to the shared ring;
     * without it the QP would draw from an empty RQ and never complete one.
     */
    if (rq_tbl_index_srq_id != 0xffffffffu)
        ionic_datapath_bind_qp_srq(ctx->dp, qp_id, rq_tbl_index_srq_id);

    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq CREATE_QP qp_id=%u qpn=%u type=%u sq_cq=%u/%u "
            "rq_cq=%u/%u pd=%u",
            qp_id, qpn, ib_qp_type, sq_cq_id, send_cq, rq_cq_id, recv_cq,
            pd_handle);
    return IONIC_RC_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Hardware statistics
 * -------------------------------------------------------------------------
 */

/*
 * STATS_HDRS, STATS_VALS, QP_STATS_HDRS and QP_STATS_VALS share one body,
 * ionic_admin_stats_hdr (17 bytes):
 *   le64 dma_addr   [0:7]
 *   le32 length     [8:11]   (the driver sends PAGE_SIZE)
 *   le32 id_ver     [12:15]  (0, or the qpid for QP_STATS_VALS)
 *   u8   type_state [16]
 *
 * A HDRS command answers with an array of struct ionic_v1_stat (be32
 * type_off, char name[28]) that ends at the first entry of type NONE; the
 * driver reads it into a zeroed page and names its hw_counters files after
 * the entries.  A VALS command answers with the values at the offsets the
 * headers gave.  Every value here is an LE64 at an 8-byte aligned offset,
 * which is what the driver's ionic_v1_stat_val() validates.
 *
 * The driver only sends these when the LIF identity advertises
 * rdma.stats_type (see ionic_eth_emu.c); until it did, a guest had no
 * hw_counters directory and these opcodes were never reached.  The layout
 * constants and the two counter tables are in ionic_adminq_priv.h, so the
 * tests can check the wire format against them.
 */

/*
 * Write @count headers and the NONE terminator, or as many whole entries as
 * @length holds.  The driver's buffer is zeroed, so a cut-off terminator
 * still reads as one.
 */
static uint8_t stats_write_hdrs(const struct ionic_adminq_ctx *ctx,
                                uint64_t dma, uint32_t length,
                                const struct adminq_stat *stats, size_t count)
{
    uint8_t buf[(STATS_MAX_COUNT + 1) * STAT_HDR_SIZE];
    size_t entries = count + 1;
    size_t fit = length / STAT_HDR_SIZE;

    if (fit == 0)
        return IONIC_RC_ENOSPC;
    if (entries > fit)
        entries = fit;

    memset(buf, 0, sizeof(buf));
    for (size_t i = 0; i < count && i < entries; i++) {
        size_t name_len = strlen(stats[i].name);

        if (name_len > STAT_NAME_LEN - 1)
            name_len = STAT_NAME_LEN - 1;
        ernic_put_be32(buf + i * STAT_HDR_SIZE,
                       STAT_TYPE_LE64 << STAT_TYPE_SHIFT |
                           (uint32_t)(i * STAT_VAL_SIZE));
        memcpy(buf + i * STAT_HDR_SIZE + 4, stats[i].name, name_len);
    }

    return ernic_dma_write(ctx->dma, ctx->vfu_ctx, dma, buf,
                           entries * STAT_HDR_SIZE) < 0
               ? IONIC_RC_EIO
               : IONIC_RC_SUCCESS;
}

/* Write the values the matching headers describe, clamped to @length. */
static uint8_t stats_write_vals(const struct ionic_adminq_ctx *ctx,
                                uint64_t dma, uint32_t length,
                                const struct adminq_stat *stats, size_t count,
                                const void *counters)
{
    uint8_t buf[STATS_MAX_COUNT * STAT_VAL_SIZE];
    size_t n = length / STAT_VAL_SIZE;

    if (n == 0)
        return IONIC_RC_ENOSPC;
    if (n > count)
        n = count;

    memset(buf, 0, sizeof(buf));
    for (size_t i = 0; i < n; i++) {
        uint64_t v;

        memcpy(&v, (const uint8_t *)counters + stats[i].off, sizeof(v));
        ernic_put_le64(buf + i * STAT_VAL_SIZE, v);
    }

    return ernic_dma_write(ctx->dma, ctx->vfu_ctx, dma, buf,
                           n * STAT_VAL_SIZE) < 0
               ? IONIC_RC_EIO
               : IONIC_RC_SUCCESS;
}

static uint8_t handle_stats_op(struct ionic_adminq_ctx *ctx, uint8_t op,
                               const uint8_t *body, uint16_t len)
{
    uint64_t dma;
    uint32_t length;
    uint32_t id_ver;

    if (len < STATS_BODY_LEN) {
        vfu_log(ctx->vfu_ctx, LOG_ERR, "ionic_adminq stats op=%u: short body",
                op);
        return IONIC_RC_EINVAL;
    }
    dma = ernic_get_le64(body);
    length = ernic_get_le32(body + 8);
    id_ver = ernic_get_le32(body + 12);
    if (!dma) {
        vfu_log(ctx->vfu_ctx, LOG_ERR, "ionic_adminq stats op=%u: no buffer",
                op);
        return IONIC_RC_BAD_ADDR;
    }

    switch (op) {
    case IONIC_V1_ADMIN_STATS_HDRS:
        return stats_write_hdrs(ctx, dma, length, dev_stats, DEV_STATS_COUNT);

    case IONIC_V1_ADMIN_QP_STATS_HDRS:
        return stats_write_hdrs(ctx, dma, length, qp_stats, QP_STATS_COUNT);

    case IONIC_V1_ADMIN_STATS_VALS: {
        struct ionic_fault *f =
            ionic_fault_set_get(ctx->faults, IONIC_FAULT_SITE_STATS_VALS);
        struct adminq_dev_vals v;

        /* Injected before the counters are read, so a guest sees the
         * failure status rather than a half-filled block. */
        if (f && ionic_fault_event(f)) {
            vfu_log(ctx->vfu_ctx, LOG_INFO,
                    "ionic_adminq STATS_VALS: injected fail status %u "
                    "(event %" PRIu64 ")",
                    (unsigned int)f->status, f->eligible);
            return f->status;
        }
        memset(&v, 0, sizeof(v));
        if (ernic_stats_dev_counters(ctx->stats, &v.stats) != 0)
            return IONIC_RC_EIO;
        ionic_datapath_rdma_counters(ctx->dp, IONIC_DP_DEVICE, v.rdma);
        return stats_write_vals(ctx, dma, length, dev_stats, DEV_STATS_COUNT,
                                &v);
    }

    case IONIC_V1_ADMIN_QP_STATS_VALS: {
        struct adminq_qp_vals v;
        uint32_t qp_id = id_ver & 0x00ffffffu;
        uint32_t qpn;

        /* Counters are keyed on the driver's qp_id; the map only proves the
         * QP exists, so a stale id fails rather than reading zeros. */
        if (!adminq_lookup_qp(ctx, qp_id, &qpn)) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq QP_STATS_VALS: unknown qp_id=%u", qp_id);
            return IONIC_RC_ENOENT;
        }
        memset(&v, 0, sizeof(v));
        if (ernic_stats_qp_counters(ctx->stats, qp_id, &v.stats) != 0)
            return IONIC_RC_EIO;
        ionic_datapath_rdma_counters(ctx->dp, qp_id, v.rdma);
        return stats_write_vals(ctx, dma, length, qp_stats, QP_STATS_COUNT, &v);
    }

    default:
        return IONIC_RC_EOPCODE;
    }
}

/* -------------------------------------------------------------------------
 * WQE dispatch
 * -------------------------------------------------------------------------
 */

/* -------------------------------------------------------------------------
 * Address handles
 * -------------------------------------------------------------------------
 */

/*
 *   le64 dma_addr      [0:7]    RoCE header template from ib_ud_header_pack
 *   le32 length        [8:11]   its length
 *   le32 pd_id         [12:15]
 *   le32 id_ver        [16:19]  ah_id in the low 24 bits
 *   le16 dbid_flags    [20:21]
 *   u8   csum_profile  [22]
 *   u8   crypto        [23]
 *
 * The template is the only statement of where the handle points; a UD send
 * WQE carries just the id. Parsing it once here is what lets the datapath
 * resolve a datagram's destination node without re-reading guest memory on
 * every send.
 */
static uint8_t handle_create_ah_op(const struct ionic_adminq_ctx *ctx,
                                   const uint8_t *body, uint16_t len)
{
    if (len < IONIC_ADMIN_CREATE_AH_IN_V1_LEN) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_AH: short body (%u)", len);
        return IONIC_RC_EINVAL;
    }

    uint64_t hdr_dma = ernic_get_le64(body + 0);
    uint32_t hdr_len = ernic_get_le32(body + 8);
    uint32_t ah_id = ernic_get_le32(body + 16) & 0x00ffffffu;

    uint8_t dgid[ERNIC_GID_LEN] = {0};
    uint8_t sgid[ERNIC_GID_LEN] = {0};
    uint8_t dmac[6] = {0};
    bool dgid_valid = false;
    if (hdr_len && hdr_dma)
        dgid_valid =
            adminq_parse_roce_hdr(ctx, hdr_dma, hdr_len, dgid, sgid, dmac);

    /*
     * An unparsable template is not fatal. A handle naming a peer inside
     * this instance needs no address -- the datagram is routed by its QPN --
     * so refusing the create would break the local case to protect the
     * remote one. Register it without a node and let the send path complain
     * if it ever needs an address it has not got.
     */
    uint32_t node =
        dgid_valid ? ionic_dp_node_from_gid(ctx->dp, dgid) : UINT32_MAX;

    ionic_datapath_register_ah(ctx->dp, ah_id, dgid_valid ? dgid : NULL,
                               dgid_valid ? sgid : NULL,
                               dgid_valid ? dmac : NULL, node);

    vfu_log(ctx->vfu_ctx, LOG_INFO, "ionic_adminq CREATE_AH %u: node %u%s",
            ah_id, node, dgid_valid ? "" : " (no address in the RoCE header)");
    return IONIC_RC_SUCCESS;
}

static uint8_t handle_destroy_ah_op(const struct ionic_adminq_ctx *ctx,
                                    const uint8_t *body, uint16_t len)
{
    if (len < IONIC_ADMIN_DESTROY_AH_IN_V1_LEN) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq DESTROY_AH: short body (%u)", len);
        return IONIC_RC_EINVAL;
    }

    uint32_t ah_id = ernic_get_le32(body + 0) & 0x00ffffffu;

    ionic_datapath_unregister_ah(ctx->dp, ah_id);
    return IONIC_RC_SUCCESS;
}

static uint8_t handle_create_mr_op(struct ionic_adminq_ctx *ctx,
                                   const uint8_t *body, uint16_t len)
{
    if (!ctx->pvrdma_handle)
        return IONIC_RC_SUCCESS;
    if (len < IONIC_ADMIN_CREATE_MR_IN_V1_LEN) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_MR: short body (%u)", len);
        return IONIC_RC_EINVAL;
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
    /* The full mrid, key byte included, is what userspace puts in an SGE
     * lkey; the low 24 bits alone are the id we track resources by. */
    uint32_t mrid = ernic_get_le32(body + 20);
    uint32_t mr_id = mrid & 0x00ffffffu;

    uint32_t access = ernic_get_le16(body + 40) & IONIC_MRF_ACCESS_MASK;
    uint64_t mr_va = ernic_get_le64(body + 0);
    uint64_t mr_len = ernic_get_le64(body + 8);

    /*
     * Refused before anything is allocated: a region that runs past the
     * end of the address space, which the data path would otherwise
     * refuse only after the MR had been allocated; an id already in use,
     * which mapped over would lose the MR that had it; and a full map,
     * which would leave the new one unmapped. Either MR could then never
     * be found again by DESTROY_MR, and would leak.
     * The driver surfaces this as the -ENOMEM its ib_alloc_mr() already
     * knows how to unwind.
     */
    if (mr_len > UINT64_MAX - mr_va) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_MR %u: %" PRIu64 " bytes at %#" PRIx64
                " run past the end of the address space",
                mr_id, mr_len, mr_va);
        return IONIC_RC_ERANGE;
    }
    uint32_t mr_handle;
    if (adminq_lookup_mr(ctx, mr_id, &mr_handle)) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_MR %u: mr_id already in use", mr_id);
        return IONIC_RC_EEXIST;
    }
    struct ionic_mr_map_entry *entry = adminq_mr_free_entry(ctx);
    if (!entry) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_MR %u: MR map full (%u MRs)", mr_id,
                MAX_MR_MAP);
        return IONIC_RC_ENOSPC;
    }

    uint32_t pd_handle;
    int ret = adminq_get_pd(ctx, &pd_handle);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq CREATE_MR %u: rdma_rm_alloc_pd failed (%d)",
                mr_id, ret);
        return IONIC_RC_ERDMA;
    }

    ret = ernic_rm_alloc_mr(ctx->pvrdma_handle, pd_handle, access, &mr_handle);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR, "ionic_adminq CREATE_MR %u: failed (%d)",
                mr_id, ret);
        return IONIC_RC_ERDMA;
    }
    struct ionic_dp_buf_desc mrbuf = {.dma_addr = ernic_get_le64(body + 32),
                                      .map_count = ernic_get_le32(body + 28),
                                      .page_size_log2 = body[44]};
    if (mr_len == 0) {
        /*
         * No region yet: ib_alloc_mr() and ib_alloc_mw() create the MR
         * empty, and a REG_MR work request binds it later. Nothing is
         * reachable through its key until then, whatever was bound to the
         * key before.
         */
        ionic_datapath_unregister_mr(ctx->dp, mrid);
    } else {
        /*
         * As for CREATE_CQ: an MR the data path has no page table for is
         * one every access through its key fails.
         */
        ret = ionic_datapath_register_mr(ctx->dp, mrid, mr_va, mr_len, &mrbuf);
        if (ret) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq CREATE_MR %u: datapath registration failed "
                    "(%d)",
                    mr_id, ret);
            ernic_rm_dealloc_mr(ctx->pvrdma_handle, mr_handle);
            return adminq_dp_status(ret);
        }
    }

    *entry = (struct ionic_mr_map_entry){
        .valid = true, .mr_id = mr_id, .handle = mr_handle};

    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq CREATE_MR mr_id=%u handle=%u access=%#x pd=%u "
            "va=%#" PRIx64 " len=%" PRIu64 " pt=%#" PRIx64 " maps=%" PRIu32
            " pgsz_log2=%u",
            mr_id, mr_handle, access, pd_handle, mr_va, mr_len, mrbuf.dma_addr,
            mrbuf.map_count, (unsigned)mrbuf.page_size_log2);
    return IONIC_RC_SUCCESS;
}

static uint8_t handle_modify_qp_op(struct ionic_adminq_ctx *ctx,
                                   const uint8_t *body, uint16_t len)
{
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
    if (!ctx->pvrdma_handle)
        return IONIC_RC_SUCCESS;
    if (len < IONIC_ADMIN_MODIFY_QP_IN_V1_LEN) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq MODIFY_QP: short body (%u)", len);
        return IONIC_RC_EINVAL;
    }

    /* The kernel's enum ib_qp_attr_mask, whose bits are those of
     * userspace's enum ibv_qp_attr_mask. */
    uint32_t attr_mask = ernic_get_be32(body + 0);

    uint32_t rq_psn, sq_psn, qkey_dest_qpn, id_ver;
    rq_psn = ernic_get_le32(body + 8);
    sq_psn = ernic_get_le32(body + 12);
    qkey_dest_qpn = ernic_get_le32(body + 16);
    id_ver = ernic_get_le32(body + 56);

    uint8_t type_state = body[39];
    uint32_t qp_id = id_ver & 0x00ffffffu;

    uint32_t qpn;
    if (!adminq_lookup_qp(ctx, qp_id, &qpn)) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq MODIFY_QP: unknown qp_id=%u", qp_id);
        return IONIC_RC_ENOENT;
    }

    /* The RoCE header template the driver DMAs for this transition is the
     * only place the destination address appears; without it every peer
     * resolves to the same default node. */
    uint32_t ah_id_len;
    bool dgid_valid = false;
    uint64_t ah_dma;
    uint8_t dgid[ERNIC_GID_LEN] = {0};
    uint8_t sgid[ERNIC_GID_LEN] = {
        0}; /* parsed but unused here; see CREATE_AH */
    uint8_t dmac[6] = {0};

    ah_id_len = ernic_get_le32(body + 32);
    ah_dma = ernic_get_le64(body + 48);

    uint32_t hdr_len = ah_id_len >> 24;
    if (hdr_len && ah_dma)
        dgid_valid =
            adminq_parse_roce_hdr(ctx, ah_dma, hdr_len, dgid, sgid, dmac);

    if (dgid_valid)
        vfu_log(ctx->vfu_ctx, LOG_DEBUG,
                "ionic_adminq MODIFY_QP %u: peer %02x:%02x:%02x:%02x:%02x:"
                "%02x gid %02x%02x:%02x%02x:%02x%02x:%02x%02x",
                qp_id, dmac[0], dmac[1], dmac[2], dmac[3], dmac[4], dmac[5],
                dgid[8], dgid[9], dgid[10], dgid[11], dgid[12], dgid[13],
                dgid[14], dgid[15]);

    /* The field as the driver wrote it, before any translation */
    const uint32_t guest_qkey_dest_qpn = qkey_dest_qpn;

    /* IBV_QP_DEST_QPN: the driver names the peer by its own qpid, but the
     * backend routes on its QPNs.  For UD this field is the qkey instead,
     * so only translate when the mask says it is a destination QPN. */
    uint32_t dest_node = UINT32_MAX;
    if (attr_mask & IBV_QP_DEST_QPN) {
        dest_node = ionic_dp_node_from_gid(ctx->dp, dgid_valid ? dgid : NULL);

        /*
         * UINT32_MAX means two different things.  With no mesh backend
         * attached -- loopback, nvmeof, s3 -- it is the sentinel for
         * "every peer is local", which the datapath relies on
         * (ionic_datapath.c, dp_is_remote()), and the destination node
         * is never consulted.  With a mesh attached it means no node
         * owns this GID, and that must fail: a wrong node there is a
         * silent write into another guest's memory that nothing
         * downstream catches.  Only the second is an error.
         */
        if (dest_node == UINT32_MAX &&
            ionic_dp_local_node(ctx->dp) != UINT32_MAX) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq MODIFY_QP %u: no mesh node owns "
                    "destination gid "
                    "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                    "%02x%02x:%02x%02x:%02x%02x:%02x%02x%s",
                    qp_id, dgid[0], dgid[1], dgid[2], dgid[3], dgid[4], dgid[5],
                    dgid[6], dgid[7], dgid[8], dgid[9], dgid[10], dgid[11],
                    dgid[12], dgid[13], dgid[14], dgid[15],
                    dgid_valid ? "" : " (no address in the RoCE header)");
            return IONIC_RC_ENOENT;
        }

        uint32_t dest_qpn;
        if (adminq_lookup_qp(ctx, qkey_dest_qpn, &dest_qpn))
            qkey_dest_qpn = dest_qpn;
    }

    int ret = ernic_rm_modify_qp(ctx->pvrdma_handle, qpn, attr_mask, type_state,
                                 sq_psn, rq_psn, qkey_dest_qpn, dgid);
    if (ret) {
        vfu_log(ctx->vfu_ctx, LOG_ERR, "ionic_adminq MODIFY_QP %u: failed (%d)",
                qp_id, ret);
        return IONIC_RC_ERDMA;
    }

    /*
     * The rest is applied only now that the modification has succeeded,
     * since one that fails must change nothing.
     *
     * IBV_QP_QKEY: the same field, read the other way. The datapath needs
     * it because a datagram is only delivered into a QP that presents
     * that QP's Q_Key -- without it the destination QPN alone selected
     * any queue in the device, including other processes' and GSI.
     */
    if (attr_mask & IBV_QP_QKEY)
        ionic_datapath_set_qkey(ctx->dp, qp_id, guest_qkey_dest_qpn);

    /* The datapath routes on the driver's qpid, and QUERY_QP reports it */
    if (attr_mask & IBV_QP_DEST_QPN) {
        struct ionic_qp_map_entry *e = adminq_qp_entry(ctx, qp_id);

        ionic_datapath_set_dest(ctx->dp, qp_id, guest_qkey_dest_qpn, dest_node);
        if (e) {
            e->dest_set = true;
            e->dest_qp_id = guest_qkey_dest_qpn;
        }
    }
    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq MODIFY_QP qp_id=%u qpn=%u type_state=%#x "
            "attr=%#x",
            qp_id, qpn, type_state, attr_mask);

    /*
     * Entering Error is the point at which outstanding work requests
     * stop being able to complete normally, so it is the point at which
     * they have to be flushed. MODIFY_QP carries the state alone in this
     * field -- unlike CREATE_QP, which packs the type into the low
     * nibble -- so the whole byte is the state here.
     *
     * IBV_QP_STATE gates it for the same reason rdma_rm_modify_qp() only
     * applies the state under that bit: without it the byte is whatever
     * the driver left there, and a command changing some other attribute
     * would flush a queue pair that never entered Error.
     */
    if ((attr_mask & IBV_QP_STATE) && type_state == IONIC_QPS_ERR)
        ionic_datapath_flush_qp(ctx->dp, qp_id);
    return IONIC_RC_SUCCESS;
}

static uint8_t handle_query_qp_op(struct ionic_adminq_ctx *ctx,
                                  const uint8_t *body, uint16_t len)
{
    /* ionic_admin_query_qp body (34 bytes):
     *   le64 hdr_dma_addr [0:7]   (AH header template, only when IB_QP_AV)
     *   le64 sq_dma_addr  [8:15]
     *   le64 rq_dma_addr  [16:23]
     *   le32 ah_id        [24:27]
     *   le32 id_ver       [28:31]  (qpid | ver<<24)
     *   le16 dbid_flags   [32:33]
     *
     * The answer is not the completion: the driver reads it out of the
     * two buffers it DMA-mapped, and both are zeroed before the command,
     * so succeeding without writing them reports a QP in RESET.  That is
     * what ibv_query_qp saw while this was a stub. */
    if (!ctx->pvrdma_handle)
        return IONIC_RC_SUCCESS;
    if (len < IONIC_ADMIN_QUERY_QP_IN_V1_LEN) {
        vfu_log(ctx->vfu_ctx, LOG_ERR, "ionic_adminq QUERY_QP: short body (%u)",
                len);
        return IONIC_RC_EINVAL;
    }

    uint64_t sq_dma, rq_dma;
    uint32_t id_ver;
    sq_dma = ernic_get_le64(body + 8);
    rq_dma = ernic_get_le64(body + 16);
    id_ver = ernic_get_le32(body + 28);

    uint32_t qp_id = id_ver & 0x00ffffffu;
    const struct ionic_qp_map_entry *qp_entry = adminq_qp_entry(ctx, qp_id);
    if (!qp_entry) {
        vfu_log(ctx->vfu_ctx, LOG_ERR,
                "ionic_adminq QUERY_QP: unknown qp_id=%u", qp_id);
        return IONIC_RC_ENOENT;
    }
    uint32_t qpn = qp_entry->qpn;

    uint8_t state = 0, path_mtu = IBV_MTU_1024;
    uint32_t dest_qpn = 0, access = 0, rq_psn = 0, sq_psn = 0;
    if (ernic_rm_query_qp(ctx->pvrdma_handle, qpn, &state, &path_mtu, &dest_qpn,
                          &access, &rq_psn, &sq_psn) != 0) {
        vfu_log(ctx->vfu_ctx, LOG_ERR, "ionic_adminq QUERY_QP %u: failed",
                qp_id);
        return IONIC_RC_ERDMA;
    }

    /*
     * The destination as the driver named it, by its qpid. The backend
     * answers in its own QPNs, which mean nothing to the guest, so its
     * answer is used only for a QP MODIFY_QP gave no destination -- the
     * loopback backend pairs those itself -- and only if it names a QP
     * of this device. 0 is the backends' "no destination".
     */
    uint32_t guest_dest = 0;
    if (qp_entry->dest_set)
        guest_dest = qp_entry->dest_qp_id;
    else if (dest_qpn == 0 || !adminq_lookup_qp_id(ctx, dest_qpn, &guest_dest))
        guest_dest = 0;

    /* ionic numbers its QP states as IB does, so the state needs no
     * translation; the MTU does, because the driver subtracts 7 from the
     * low nibble to recover the ibv_mtu enum. */
    uint16_t flags = 0;
    if (access & IBV_ACCESS_REMOTE_WRITE)
        flags |= 1u << 0; /* IONIC_QPF_REMOTE_WRITE */
    if (access & IBV_ACCESS_REMOTE_READ)
        flags |= 1u << 1; /* IONIC_QPF_REMOTE_READ */
    if (access & IBV_ACCESS_REMOTE_ATOMIC)
        flags |= 1u << 2; /* IONIC_QPF_REMOTE_ATOMIC */

    /* struct ionic_v1_admin_query_qp_sq, all big-endian (20 bytes). */
    uint8_t sqbuf[20] = {0};
    ernic_put_be16(sqbuf + 2, flags);
    ernic_put_be32(sqbuf + 8, guest_dest);
    ernic_put_be32(sqbuf + 16, rq_psn & 0xffffffu);

    /* struct ionic_v1_admin_query_qp_rq (12 bytes).  rrq/rsq depth are
     * log2 depths: the driver reports BIT(depth) - 1 as the rd_atomic
     * limits, so 1 is the single outstanding operation ionic_datapath
     * actually supports. */
    uint8_t rqbuf[12] = {0};
    rqbuf[0] = (uint8_t)((state & 0x0fu) << 4 | ((path_mtu + 7u) & 0x0fu));
    rqbuf[1] = 0x77; /* retry_cnt 7, rnr_retry 7 */
    rqbuf[2] = 1;
    rqbuf[3] = 1;
    ernic_put_be32(rqbuf + 4, sq_psn & 0xffffffu);
    ernic_put_be16(rqbuf + 8, flags);

    if (sq_dma && ernic_dma_write(ctx->dma, ctx->vfu_ctx, sq_dma, sqbuf,
                                  sizeof(sqbuf)) < 0)
        return IONIC_RC_EIO;
    if (rq_dma && ernic_dma_write(ctx->dma, ctx->vfu_ctx, rq_dma, rqbuf,
                                  sizeof(rqbuf)) < 0)
        return IONIC_RC_EIO;

    vfu_log(ctx->vfu_ctx, LOG_INFO,
            "ionic_adminq QUERY_QP qp_id=%u qpn=%u state=%u dest=%u "
            "(backend dest_qpn=%u)",
            qp_id, qpn, state, guest_dest, dest_qpn);
    return IONIC_RC_SUCCESS;
}

uint8_t ionic_adminq_dispatch_wqe(struct ionic_adminq_ctx *ctx, uint8_t op,
                                  const uint8_t *body, uint16_t len)
{
    vfu_log(ctx->vfu_ctx, LOG_INFO, "ionic_adminq: WQE op=%u len=%u", op, len);

    ctx->resp_len = 0;

    switch (op) {
    case IONIC_V1_ADMIN_NOOP:
        return IONIC_RC_SUCCESS;

    case IONIC_V1_ADMIN_CREATE_CQ:
        return handle_create_cq_op(ctx, body, len);

    case IONIC_V1_ADMIN_CREATE_QP:
        return handle_create_qp_op(ctx, body, len);

    case IONIC_V1_ADMIN_CREATE_SRQ:
        return handle_create_srq_op(ctx, body, len);

    case IONIC_V1_ADMIN_MODIFY_SRQ:
        return handle_modify_srq_op(ctx, body, len);

    case IONIC_V1_ADMIN_QUERY_SRQ:
        return handle_query_srq_op(ctx, body, len);

    case IONIC_V1_ADMIN_DESTROY_SRQ:
        return handle_destroy_srq_op(ctx, body, len);

    case IONIC_V1_ADMIN_CREATE_MR:
        return handle_create_mr_op(ctx, body, len);

    case IONIC_V1_ADMIN_DESTROY_MR: {
        if (!ctx->pvrdma_handle)
            return IONIC_RC_SUCCESS;
        if (len < IONIC_ADMIN_DESTROY_MR_IN_V1_LEN) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq DESTROY_MR: short body (%u)", len);
            return IONIC_RC_EINVAL;
        }
        uint32_t mrid, mr_handle;
        mrid = ernic_get_le32(body);
        uint32_t mr_id = mrid & 0x00ffffffu;
        ionic_datapath_unregister_mr(ctx->dp, mrid);
        /* A miss now means the resource is stranded: nothing else holds the
         * handle.  It used to be reported as success, which is how the table
         * filled up without a single line in the log. */
        if (!adminq_lookup_mr(ctx, mr_id, &mr_handle)) {
            vfu_log(ctx->vfu_ctx, LOG_WARNING,
                    "ionic_adminq DESTROY_MR %u: no mapping; resource leaked",
                    mr_id);
            return IONIC_RC_ENOENT;
        }
        ernic_rm_dealloc_mr(ctx->pvrdma_handle, mr_handle);
        adminq_unmap_mr(ctx, mr_id);
        return IONIC_RC_SUCCESS;
    }

    case IONIC_V1_ADMIN_DESTROY_CQ: {
        if (!ctx->pvrdma_handle)
            return IONIC_RC_SUCCESS;
        if (len < IONIC_ADMIN_DESTROY_CQ_IN_V1_LEN) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq DESTROY_CQ: short body (%u)", len);
            return IONIC_RC_EINVAL;
        }
        uint32_t cq_id, cq_handle;
        cq_id = ernic_get_le32(body);
        ionic_datapath_unregister_cq(ctx->dp, cq_id);
        /* Never created here, so nothing to release */
        if (!adminq_lookup_cq(ctx, cq_id, &cq_handle))
            return IONIC_RC_SUCCESS;
        ernic_rm_dealloc_cq(ctx->pvrdma_handle, cq_handle);
        adminq_unmap_cq(ctx, cq_id);
        return IONIC_RC_SUCCESS;
    }

    case IONIC_V1_ADMIN_DESTROY_QP: {
        if (!ctx->pvrdma_handle)
            return IONIC_RC_SUCCESS;
        if (len < IONIC_ADMIN_DESTROY_QP_IN_V1_LEN) {
            vfu_log(ctx->vfu_ctx, LOG_ERR,
                    "ionic_adminq DESTROY_QP: short body (%u)", len);
            return IONIC_RC_EINVAL;
        }
        uint32_t qp_id, qpn;
        qp_id = ernic_get_le32(body);
        ionic_datapath_unregister_qp(ctx->dp, qp_id);
        /* Per-QP stats are keyed on the driver-side qp_id, not the resource
         * manager's qpn, so drop them here while qp_id is still in hand. */
        ernic_stats_qp_forget(ctx->stats, qp_id);
        if (!adminq_lookup_qp(ctx, qp_id, &qpn))
            return IONIC_RC_SUCCESS;
        ernic_rm_dealloc_qp(ctx->pvrdma_handle, qpn);
        adminq_unmap_qp(ctx, qp_id);
        return IONIC_RC_SUCCESS;
    }

    case IONIC_V1_ADMIN_MODIFY_QP:
        return handle_modify_qp_op(ctx, body, len);

    case IONIC_V1_ADMIN_QUERY_QP:
        return handle_query_qp_op(ctx, body, len);

    case IONIC_V1_ADMIN_CREATE_AH:
        return handle_create_ah_op(ctx, body, len);

    case IONIC_V1_ADMIN_DESTROY_AH:
        return handle_destroy_ah_op(ctx, body, len);

    case IONIC_V1_ADMIN_QUERY_AH:
        /* Still a stub, and harmlessly so: the driver only issues it to read
         * back a header template it already has, and nothing here consumes
         * the result. CREATE_AH is the one that had to be real. */
        (void)body;
        (void)len;
        vfu_log(ctx->vfu_ctx, LOG_DEBUG,
                "ionic_adminq: QUERY_AH stub (succeeds)");
        return IONIC_RC_SUCCESS;

    case IONIC_V1_ADMIN_STATS_HDRS:
    case IONIC_V1_ADMIN_STATS_VALS:
    case IONIC_V1_ADMIN_QP_STATS_HDRS:
    case IONIC_V1_ADMIN_QP_STATS_VALS:
        return handle_stats_op(ctx, op, body, len);

    case IONIC_V1_ADMIN_MODIFY_DCQCN:
    case IONIC_V1_ADMIN_DEBUG:
        return IONIC_RC_SUCCESS; /* benign stub */

    default:
        vfu_log(ctx->vfu_ctx, LOG_WARNING, "ionic_adminq: unknown op=%u", op);
        return IONIC_RC_EOPCODE;
    }
}

/* -------------------------------------------------------------------------
 * Poll loop — called from server main loop
 * -------------------------------------------------------------------------
 */

/*
 * Read the strides after the first of the @strides-stride WQE at @slot into
 * @buf, after the first; none when @strides is 1. They are the next ring
 * entries, so a WQE that reaches the end of the ring continues at slot 0;
 * reading on past the end would take guest memory that isn't the ring.
 * Returns 0 or the failed ernic_dma_read()'s error.
 */
static int read_wqe_tail(struct ernic_dma *dma, vfu_ctx_t *vfu_ctx,
                         const struct ionic_aq_ring *r, uint32_t slot,
                         uint32_t strides, uint8_t *buf)
{
    uint32_t next = (slot + 1u) % r->aq_depth;
    uint32_t left = strides - 1u;
    uint8_t *dst = buf + ADMIN_WQE_STRIDE;

    while (left > 0) {
        uint32_t run = r->aq_depth - next;
        if (run > left)
            run = left;
        uint64_t gpa = r->aq_dma + (uint64_t)next * ADMIN_WQE_STRIDE;
        int ret = ernic_dma_read(dma, vfu_ctx, gpa, dst,
                                 (size_t)run * ADMIN_WQE_STRIDE);
        if (ret < 0)
            return ret;
        dst += (size_t)run * ADMIN_WQE_STRIDE;
        left -= run;
        next = 0; /* anything left wrapped */
    }
    return 0;
}

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

            /* The header is [0:3] and the body follows it. Most ops fit in
             * one stride, so read the rest only when the WQE has more, to
             * avoid an extra DMA round-trip.
             *
             * Zeroed as defense in depth only: the length check below is
             * what keeps a handler from seeing bytes that weren't read, but
             * if that ever breaks, it sees zeros rather than host stack. */
            uint8_t wqe_buf[ADMIN_WQE_MAX_STRIDES * ADMIN_WQE_STRIDE] = {0};
            if (ernic_dma_read(ctx->dma, vfu_ctx, wqe_gpa, wqe_buf,
                               ADMIN_WQE_STRIDE) < 0)
                break;

            uint8_t op = wqe_buf[0];
            uint16_t len = ernic_get_le16(wqe_buf + 2);

            /* Counted as the driver counts them: CREATE_QP's 64-byte body
             * and the header are 68 bytes, so 2 strides. */
            uint32_t total_bytes = ADMIN_WQE_HDR_LEN + (uint32_t)len;
            uint32_t strides =
                (total_bytes + ADMIN_WQE_STRIDE - 1u) / ADMIN_WQE_STRIDE;
            uint32_t avail = pending - processed;

            /*
             * The handlers take len as the size of the body, so all of it
             * has to have been read. Refuse a WQE longer than wqe_buf, or
             * one that runs past the producer index (the driver rings the
             * doorbell only once a WQE's last stride is written), rather
             * than run it on bytes the guest never wrote there. Skip the
             * strides len claims, as the driver does when it completes, but
             * never past the producer index.
             */
            uint8_t status;
            if (strides > ADMIN_WQE_MAX_STRIDES || strides > avail) {
                vfu_log(ctx->vfu_ctx, LOG_ERR,
                        "ionic_adminq: refusing WQE op=%u len=%u at slot %u: "
                        "%u strides, %u produced (max %u)",
                        op, len, slot, strides, avail, ADMIN_WQE_MAX_STRIDES);
                status = IONIC_RC_EINVAL;
                if (strides > avail)
                    strides = avail;
            } else {
                if (read_wqe_tail(ctx->dma, vfu_ctx, r, slot, strides,
                                  wqe_buf) < 0)
                    break;
                status = ionic_adminq_dispatch_wqe(
                    ctx, op, wqe_buf + ADMIN_WQE_HDR_LEN, len);
            }

            /* No information lost: the ring is at most
             * 2^IONIC_ADMINQ_MAX_DEPTH_LOG2 deep. */
            uint16_t cmd_idx = (uint16_t)slot;
            ionic_adminq_post_cqe(ctx, r, cmd_idx, op, status, ctx->resp_len);
            ernic_stats_command(ctx->stats);

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
