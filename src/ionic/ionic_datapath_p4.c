/*
 * ionic_datapath_p4.c — the data path's side of the P4 pipeline
 *
 * When a P4 program is loaded, every send work request runs through the
 * pipeline after its memory has been checked and before it is dispatched
 * (ionic_dp_p4_policy()).  A program can deny the operation, which fails
 * it, and its fp8 extern can run an arithmetic or a format conversion over
 * registered guest memory in place of the transfer.  Built only with
 * ERNIC_BUILD_P4.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <vfio-user/libvfio-user.h>

#include "byteorder.h"
#include "ionic/ionic_datapath_fw.h"
#include "ionic/ionic_datapath_p4.h"
#include "ionic/ionic_datapath_priv.h"
#include "p4/p4_engine.h"
#include "p4/p4_exec.h"
#include "p4/p4_fp8.h"
#include "p4/p4_fmt.h"
#include "p4/p4_program.h"
#include "p4/p4_value.h"

void ionic_datapath_set_p4_engine(struct ionic_datapath *dp,
                                  struct p4_engine *eng)
{
    if (dp)
        dp->p4_engine = eng;
}

/*
 * Blocks the operands are streamed through.  Small enough that three of
 * them on the stack of a function on the poll-loop thread is unremarkable,
 * large enough that a megabyte moves in a few dozen passes.
 */
#define FP8_MAP_CHUNK 2048u

/*
 * Is @key one the guest may name as an operand here?
 *
 * Key 0 is IONIC_DMA_LKEY, which means "the va is already a bus address"
 * and bypasses the memory region table entirely -- ionic_dp_mr_covers() returns
 * true for it without looking anything up.  That is a deliberate internal
 * convention, and it is not one a guest-supplied compute request may use:
 * it would let the request name any memory the VMM exported rather than
 * only what the guest registered.
 */
static bool fp8_key_ok(struct ionic_datapath *dp, uint32_t key, uint64_t va,
                       uint32_t len)
{
    return key != 0 && ionic_dp_mr_covers(dp, key, va, len);
}

int ionic_datapath_fp8_map(struct ionic_datapath *dp, uint8_t op,
                           uint32_t a_lkey, uint64_t a_va, uint32_t b_lkey,
                           uint64_t b_va, uint32_t len, uint32_t c_rkey,
                           uint64_t c_va)
{
    /*
     * Both operands are staged a block at a time rather than combined in
     * place.  They are independently scattered across guest pages and the
     * operation has to see matching elements of each, which a page-at-a-
     * time walk of two separate scatter lists does not give.
     */
    uint8_t a[FP8_MAP_CHUNK];
    uint8_t b[FP8_MAP_CHUNK];
    uint8_t c[FP8_MAP_CHUNK];
    uint32_t done = 0;

    if (!dp || len == 0 || len > IONIC_FP8_MAP_MAX)
        return -EINVAL;

    /*
     * Every region is checked in full before a byte moves.  Checking as we
     * go would leave a half-written destination behind when the tail of an
     * operand turned out not to be registered, which is the transfer-then-
     * report pattern the rest of the data path was fixed away from.
     */
    if (!fp8_key_ok(dp, a_lkey, a_va, len) ||
        !fp8_key_ok(dp, b_lkey, b_va, len) ||
        !fp8_key_ok(dp, c_rkey, c_va, len))
        return -EACCES;

    while (done < len) {
        uint32_t n = len - done;

        if (n > FP8_MAP_CHUNK)
            n = FP8_MAP_CHUNK;

        if (ionic_dp_sge_to_host(dp, a_lkey, a_va + done, a, n) != n ||
            ionic_dp_sge_to_host(dp, b_lkey, b_va + done, b, n) != n)
            return -EFAULT;

        if (!p4_fp8_map((enum p4_fp8_op)op, a, b, n, c))
            return -EINVAL;

        /* Out through the same path an RDMA WRITE takes, so the result
         * reaches the destination the way any other payload would. */
        if (ionic_dp_host_to_sge(dp, c_rkey, c_va + done, c, n) != n)
            return -EFAULT;

        done += n;
    }

    return 0;
}

/*
 * Elements converted per pass.  Even, so a 4-bit format's chunk always
 * lands on a byte boundary at both ends, and small enough that the two
 * staging buffers -- at most four bytes an element each way -- are an
 * unremarkable amount of stack on the poll-loop thread.
 */
#define FMT_CVT_CHUNK_ELEMS 512u

int ionic_datapath_fmt_convert(struct ionic_datapath *dp, uint8_t src_fmt,
                               uint8_t dst_fmt, uint32_t a_lkey, uint64_t a_va,
                               uint32_t in_len, uint32_t c_rkey, uint64_t c_va)
{
    uint8_t in[FMT_CVT_CHUNK_ELEMS * 4u];
    uint8_t out[FMT_CVT_CHUNK_ELEMS * 4u];
    size_t n_elems, out_len;
    uint32_t done = 0;

    if (!dp || in_len == 0)
        return -EINVAL;
    if (!p4_fmt_valid(src_fmt) || !p4_fmt_valid(dst_fmt))
        return -EINVAL;

    /*
     * A source length that is not a whole number of elements, or an
     * element count the destination cannot hold in whole bytes, is
     * malformed rather than merely unsupported: a half element at either
     * end is not an array of either format.
     */
    n_elems = p4_fmt_elems((enum p4_fmt)src_fmt, in_len);
    if (n_elems == 0)
        return -EINVAL;
    if (n_elems > IONIC_FP8_MAP_MAX)
        return -EINVAL;
    out_len = p4_fmt_bytes((enum p4_fmt)dst_fmt, n_elems);
    if (out_len == 0)
        return -EINVAL;

    /* Each region against its own length; they differ here. */
    if (!fp8_key_ok(dp, a_lkey, a_va, in_len) ||
        !fp8_key_ok(dp, c_rkey, c_va, (uint32_t)out_len))
        return -EACCES;

    while (done < n_elems) {
        size_t n = n_elems - done;
        size_t in_bytes, out_bytes;

        if (n > FMT_CVT_CHUNK_ELEMS)
            n = FMT_CVT_CHUNK_ELEMS;
        in_bytes = p4_fmt_bytes((enum p4_fmt)src_fmt, n);
        out_bytes = p4_fmt_bytes((enum p4_fmt)dst_fmt, n);

        if (ionic_dp_sge_to_host(
                dp, a_lkey, a_va + p4_fmt_bytes((enum p4_fmt)src_fmt, done), in,
                (uint32_t)in_bytes) != in_bytes)
            return -EFAULT;

        if (!p4_fmt_convert((enum p4_fmt)src_fmt, (enum p4_fmt)dst_fmt, in, n,
                            out))
            return -EINVAL;

        if (ionic_dp_host_to_sge(
                dp, c_rkey, c_va + p4_fmt_bytes((enum p4_fmt)dst_fmt, done),
                out, (uint32_t)out_bytes) != out_bytes)
            return -EFAULT;

        done += (uint32_t)n;
    }

    return 0;
}

/*
 * The work request did not ask for a compute operation at all.
 *
 * Distinct from a malformed one, and the distinction matters: a program's
 * table is keyed on things like the opcode, which cannot tell a reduction
 * apart from an ordinary RDMA WRITE, so the action fires for both.  A
 * request that is not the two-equal-operand shape has not asked for
 * arithmetic, and failing it would take ordinary RDMA WRITE away from
 * every QP on a device whose guest installed a reduce entry.
 *
 * ENOEXEC is used because nothing else in the engine or the datapath
 * returns it, so it cannot be confused with a real failure from the
 * pipeline.
 */
#define DP_P4_NOT_COMPUTE (-ENOEXEC)

/*
 * What an extern called from the pipeline is given.
 *
 * Everything an operation may touch is in here, and the pipeline cannot
 * see any of it: the program chooses the verb and the architecture
 * supplies the operands.  @ran records that an extern did work, which the
 * caller needs in order to refuse an operation that would then also
 * transfer over the result.
 */
struct dp_p4_ctx {
    struct ionic_datapath *dp;
    const struct dp_sge_list *src;
    const uint8_t *wqe;
    uint32_t read_sz;
    uint32_t qp_id;
    uint8_t op;
    bool ran;
};

/*
 * The operand convention for a compute operation.
 *
 * An RDMA WRITE whose gather list has exactly two equal-length entries:
 * those are the two operands, the WQE's rkey and virtual address are the
 * destination, and the WQE's length is how long each of the three is.
 * That is the three-region shape a collective wants, expressed entirely in
 * things the guest had to register and post for itself.
 *
 * It is fixed here rather than carried in the P4 program on purpose.  A
 * table entry that could name a key and an address would be an addressing
 * authority over guest memory; what a program may say is which arithmetic
 * to apply, and @sel is the whole of it.
 */
static int dp_p4_fp8_map(const struct dp_p4_ctx *c, uint8_t sel)
{
    /*
     * Neither of these is an error: they say the request is an ordinary
     * transfer, not a compute request.  See DP_P4_NOT_COMPUTE.
     */
    if (c->op != IONIC_V1_OP_RDMA_WRITE && c->op != IONIC_V1_OP_RDMA_WRITE_IMM)
        return DP_P4_NOT_COMPUTE;
    if (c->src->count != 2 || c->src->len[0] != c->src->len[1])
        return DP_P4_NOT_COMPUTE;

    if (c->read_sz < WQE_SEND_LEN_OFF + 4u)
        return -EINVAL;

    uint64_t remote_va = ernic_get_be64(c->wqe + WQE_RDMA_VA_OFF);
    uint32_t rkey = ernic_get_be32(c->wqe + WQE_RDMA_RKEY_OFF);
    uint32_t length = ernic_get_be32(c->wqe + WQE_SEND_LEN_OFF);

    /*
     * The work request's length is the whole gather list, because that
     * is what a driver puts there: ionic_prep_common() sums the scatter
     * list into wqe->common.length, so a two-operand request says 2N and
     * cannot be made to say anything else from a verbs caller.  The
     * result is elementwise, so the destination is N -- half of it.
     *
     * Checking the sum rather than the per-operand length is what makes
     * this reachable at all.  An earlier version demanded the WQE say N,
     * which the in-process harness could do, because it sets the field
     * directly, and which no real guest can.
     */
    if (length != c->src->total)
        return -EINVAL;

    return ionic_datapath_fp8_map(c->dp, sel, c->src->lkey[0], c->src->va[0],
                                  c->src->lkey[1], c->src->va[1],
                                  c->src->len[0], rkey, remote_va);
}

/*
 * The operand convention for a conversion.
 *
 * An RDMA WRITE whose gather list has exactly ONE entry: that is the
 * source, and the WQE's rkey and virtual address are the destination.
 * One entry rather than two is what tells a conversion from a reduction,
 * and it is the shape a driver produces for an ordinary one-buffer write,
 * so a program that selects a conversion gets it from a work request the
 * guest could already post.
 *
 * The destination may be longer than the source, shorter, or the same --
 * the program names both formats and either may be the wider.  That is
 * the one place the architecture's "three regions of length N" does not
 * hold, so the length is not checked against the destination here;
 * ionic_datapath_fmt_convert() checks each region against its own.
 */
static int dp_p4_fmt_convert(const struct dp_p4_ctx *c, uint8_t src_fmt,
                             uint8_t dst_fmt)
{
    /* As in dp_p4_fp8_map(): neither of these is an error, they say the
     * request is an ordinary transfer rather than a conversion. */
    if (c->op != IONIC_V1_OP_RDMA_WRITE && c->op != IONIC_V1_OP_RDMA_WRITE_IMM)
        return DP_P4_NOT_COMPUTE;
    if (c->src->count != 1)
        return DP_P4_NOT_COMPUTE;

    if (c->read_sz < WQE_SEND_LEN_OFF + 4u)
        return -EINVAL;

    uint64_t remote_va = ernic_get_be64(c->wqe + WQE_RDMA_VA_OFF);
    uint32_t rkey = ernic_get_be32(c->wqe + WQE_RDMA_RKEY_OFF);
    uint32_t length = ernic_get_be32(c->wqe + WQE_SEND_LEN_OFF);

    /* The driver sums the gather list into the length field; with one
     * entry that is the source length, and it has to agree. */
    if (length != c->src->total)
        return -EINVAL;

    return ionic_datapath_fmt_convert(c->dp, src_fmt, dst_fmt, c->src->lkey[0],
                                      c->src->va[0], c->src->len[0], rkey,
                                      remote_va);
}

static int dp_p4_extern_call(void *ctx, uint8_t type, uint8_t method,
                             const struct p4_value *args, uint8_t nargs)
{
    struct dp_p4_ctx *c = ctx;

    if (type != P4_EXTERN_FP8)
        return -ENOSYS;
    /* map(op) names one arithmetic; cvt(src, dst) names two formats. */
    if (method == P4_EXTERN_FP8_MAP) {
        if (nargs != 1)
            return -ENOSYS;
    } else if (method == P4_EXTERN_FP8_CVT) {
        if (nargs != 2)
            return -ENOSYS;
    } else {
        return -ENOSYS;
    }

    /*
     * Each selector is one byte of a value that is wide enough for any
     * field the loader accepts.  A program that declared one wider and
     * put something in the top is not selecting an operation we have, so
     * it is refused rather than truncated.  Every argument is checked,
     * not just the first: cvt() has two.
     */
    for (uint8_t a = 0; a < nargs; a++)
        for (size_t i = 0; i + 1 < P4_VALUE_BYTES; i++)
            if (args[a].b[i])
                return -EINVAL;

    int rc;

    if (method == P4_EXTERN_FP8_MAP)
        rc = dp_p4_fp8_map(c, args[0].b[P4_VALUE_BYTES - 1]);
    else
        rc = dp_p4_fmt_convert(c, args[0].b[P4_VALUE_BYTES - 1],
                               args[1].b[P4_VALUE_BYTES - 1]);
    if (rc == 0)
        c->ran = true;
    return rc;
}

/*
 * The program dropped an operation that no extern ran for: it denied it.
 *
 * Nothing is transferred, and the work request fails with a local QP
 * operation error.  The refusal is this NIC's own, made before anything
 * reaches a wire, so the error is local, as a protection error found in
 * the gather list is, and it is the same on every transport: a UD send
 * the program dropped was never sent, not lost on the way.
 *
 * Completing it successfully would tell an RC sender that the peer had
 * its data, and leave the peer waiting for a write that never comes, with
 * nothing anywhere to say why.  The error completion ends the QP for the
 * consumer, as any error does, and the log says whose decision it was,
 * which the status alone cannot.
 */
static uint32_t dp_p4_denied(struct ionic_datapath *dp, uint32_t qp_id,
                             uint8_t op, bool *done)
{
    if (ionic_dp_log_ok(dp, &dp->p4_log_denied, "P4 denial(s)"))
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u op %u denied by the P4 program", qp_id,
                op);
    *done = true;
    return IONIC_STS_LOCAL_QP_OPER_ERR;
}

/*
 * Run the pipeline over one send WQE.
 *
 * Returns the completion status, and sets *@done when the ordinary
 * dispatch must not run: because an extern did the operation in place of
 * the transfer, or because the status is an error, a denial included.
 */
uint32_t ionic_dp_p4_policy(struct ionic_datapath *dp, uint32_t qp_id,
                            uint8_t op, const uint8_t *wqe, uint32_t read_sz,
                            const struct dp_sge_list *src, uint32_t imm_be,
                            bool *done)
{
    struct dp_p4_ctx ctx = {
        .dp = dp,
        .src = src,
        .wqe = wqe,
        .read_sz = read_sz,
        .qp_id = qp_id,
        .op = op,
        .ran = false,
    };
    const struct p4_extern_ops ops = {
        .call = dp_p4_extern_call,
        .ctx = &ctx,
    };
    struct p4_rdma_meta meta;
    bool suppress = false;

    /* Goes nowhere near a wire, but it is the pipeline's whole view of the
     * operation, so no field of it is left to whatever was on the stack. */
    memset(&meta, 0, sizeof(meta));
    meta.opcode = op;
    meta.qp_num = qp_id;
    meta.length = src->total;
    meta.imm_data = ernic_get_be32((const uint8_t *)&imm_be);
    if ((op == IONIC_V1_OP_RDMA_WRITE || op == IONIC_V1_OP_RDMA_WRITE_IMM ||
         op == IONIC_V1_OP_RDMA_READ) &&
        read_sz >= WQE_RDMA_RKEY_OFF + 4u) {
        meta.rkey = ernic_get_be32(wqe + WQE_RDMA_RKEY_OFF);
    }

    int rc = p4_engine_process_rdma(dp->p4_engine, &ops, &meta, &suppress);

    /*
     * No program, or one that was never meant for this path, is not a
     * refusal: the device behaves as it does with no pipeline at all.
     * Every other error is, because the pipeline is the policy and
     * carrying on past a failed one is exactly bypassing it.
     */
    if (rc == -ENOENT)
        return IONIC_STS_OK;
    /*
     * The program selected an arithmetic for a request that is not a
     * compute request -- an ordinary RDMA WRITE, most of the time, because
     * a table keyed on the opcode cannot tell the two apart.  The extern
     * wrote nothing and the pipeline stopped at the call, so the call
     * decided nothing about this operation: let it dispatch as it would
     * with no program at all.
     *
     * Failing here instead is what made a guest that installed a reduce
     * entry unable to do an ordinary RDMA WRITE on any QP of the device.
     *
     * Only when NOTHING ran, though.  An action may call the extern more
     * than once -- cvt() and then map(), say -- and the first call can
     * have written the destination before the second declined.  Falling
     * through then would put the ordinary transfer on top of a result
     * that is already there, and report success, which is the very thing
     * the ctx.ran check below exists to prevent.  An operation that got
     * that far is failed instead.
     *
     * And what the program decided BEFORE the call still stands.  A
     * mark_to_drop() ahead of it -- earlier in the action, or in an
     * earlier table, such as an access list in front of the compute
     * table -- is a drop, and the engine still reports it.  Dispatching
     * the write anyway would let a request the extern declines bypass
     * the program's explicit denial.  It fails as a denial, exactly as it
     * would with no extern call.
     */
    if (rc == DP_P4_NOT_COMPUTE && !ctx.ran) {
        if (suppress)
            return dp_p4_denied(dp, qp_id, op, done);
        return IONIC_STS_OK;
    }
    if (rc < 0) {
        if (ionic_dp_log_ok(dp, &dp->p4_log_failed, "P4 pipeline failure(s)"))
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u op %u P4 pipeline failed (%d)",
                    qp_id, op, rc);
        *done = true;
        return IONIC_STS_LOCAL_QP_OPER_ERR;
    }

    /*
     * An extern that wrote the destination and an operation that then also
     * transfers would both write the same region, and the ordinary
     * transfer would land second: the guest would get its raw payload
     * where it asked for a result, with nothing to say so.  A program
     * whose action calls the extern has to mark_to_drop() as well, and one
     * that forgets is refused rather than quietly producing that.
     *
     * The extern has already run by this point, so the destination holds
     * the result: what the error prevents is the transfer landing on top
     * of it, and what it tells the guest is not to trust the region.
     */
    if (ctx.ran && !suppress) {
        if (ionic_dp_log_ok(dp, &dp->p4_log_no_drop,
                            "P4 extern call(s) without a drop"))
            vfu_log(dp->vfu_ctx, LOG_WARNING,
                    "ionic_datapath: QP %u op %u P4 program called an extern "
                    "but did not suppress the operation; the transfer would "
                    "overwrite the result",
                    qp_id, op);
        *done = true;
        return IONIC_STS_LOCAL_QP_OPER_ERR;
    }

    /* An extern that ran and a drop: the extern did the operation. */
    if (ctx.ran) {
        *done = true;
        return IONIC_STS_OK;
    }
    if (suppress)
        return dp_p4_denied(dp, qp_id, op, done);
    return IONIC_STS_OK;
}
