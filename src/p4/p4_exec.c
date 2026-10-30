/*
 * p4_exec.c — the pipeline executor
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <string.h>

#include "p4/p4_exec.h"
#include "p4/p4_table.h"
#include "p4/p4_value.h"
#include "p4/p4_program.h"

/*
 * The packet header vector: one contiguous byte range per header instance,
 * at the offset the loader gave it (p4_header.phv_offset), plus a validity
 * bit each.  Fields are addressed by bit offset within their header,
 * exactly as they arrived on the wire, so extraction is a copy and
 * deparsing is the same copy back.  Only the program's phv_bytes of @data
 * are initialised, and nothing reads past them.
 */
struct p4_phv {
    uint8_t data[P4_PHV_BYTES];
    bool valid[P4_MAX_HEADERS];
    /*
     * Whether anything the program ran assigned standard_metadata's
     * egress_spec.  A program that never does has expressed no opinion
     * about where the frame goes, which is not the same as asking for
     * port 0 -- see p4_engine_process().  Cleared as egress starts
     * (enter_egress()), since only ingress chooses the port.
     */
    bool egress_assigned;
};

/* Bytes a header occupies, rounded up from its width in bits. */
static uint16_t hdr_bytes(const struct p4_program *prog, p4_idx_t h)
{
    return (
        uint16_t)((prog->header_types[prog->headers[h].header_type].total_bits +
                   7U) /
                  8U);
}

/*
 * Start a packet: every header invalid and zero.  The loader laid the
 * headers out and refused a program they don't fit, so only the bytes the
 * program uses need clearing.
 */
static int phv_init(const struct p4_program *prog, struct p4_phv *phv)
{
    /* A backstop: the loader never produces a program this is true of. */
    if (prog->phv_bytes > P4_PHV_BYTES) {
        return -ENOSPC;
    }
    memset(phv->data, 0, prog->phv_bytes);
    memset(phv->valid, 0, sizeof(phv->valid));
    phv->egress_assigned = false;
    return 0;
}

static const struct p4_field *field_of(const struct p4_program *prog,
                                       struct p4_field_ref ref)
{
    return &prog->header_types[prog->headers[ref.header].header_type]
                .fields[ref.field];
}

/* Where field @f, which @ref names, starts in the PHV, in bits. */
static uint32_t phv_bit(const struct p4_program *prog, struct p4_field_ref ref,
                        const struct p4_field *f)
{
    return ((uint32_t)prog->headers[ref.header].phv_offset * 8U) +
           f->offset_bits;
}

static void phv_get(const struct p4_program *prog, const struct p4_phv *phv,
                    struct p4_field_ref ref, struct p4_value *out)
{
    const struct p4_field *f = field_of(prog, ref);

    bits_get(phv->data, phv_bit(prog, ref, f), f->bits, out);
}

static void phv_set(const struct p4_program *prog, struct p4_phv *phv,
                    struct p4_field_ref ref, const struct p4_value *v)
{
    const struct p4_field *f = field_of(prog, ref);

    /* Every assignment the program makes comes through here, so this is
     * the one place that can see egress_spec being written. */
    if (ref.header == prog->egress_spec.header &&
        ref.field == prog->egress_spec.field) {
        phv->egress_assigned = true;
    }

    bits_set(phv->data, phv_bit(prog, ref, f), f->bits, v);
}

/*
 * A literal from the program, as a value.
 *
 * ernic_parse_hexstr() stores a value right-aligned in the whole buffer it
 * was given and reports separately how many bytes the digits needed, so
 * the significant bytes are the LAST @len of @cap -- not the first.
 * Reading from the front yields zero for every literal narrower than the
 * buffer.
 */
static void imm_value(const uint8_t *imm, uint16_t cap, uint16_t len,
                      struct p4_value *out)
{
    uint16_t n = (len > cap) ? cap : len;

    value_zero(out);
    if (n > P4_VALUE_BYTES) {
        n = P4_VALUE_BYTES;
    }
    for (uint16_t i = 0; i < n; i++) {
        out->b[P4_VALUE_BYTES - 1U - i] = imm[cap - 1U - i];
    }
}

/* ---- the parser -------------------------------------------------------- */

/*
 * Parse @frame into @phv.  On success *@consumed is how far the parser read,
 * which is where the payload starts.  That is the parser's position, not
 * the total of the headers left valid: a header extracted twice is valid
 * once but took bytes twice, and in bmv2 the payload starts after both.
 */
static int run_parser(const struct p4_program *prog, struct p4_phv *phv,
                      const uint8_t *frame, size_t len, size_t *consumed)
{
    p4_idx_t state = prog->init_state;
    size_t pos = 0;
    /* A parse state graph may legitimately revisit a state, so progress is
     * bounded by steps rather than by a visited set. */
    unsigned steps = 0;
    const unsigned max_steps = ((unsigned)prog->nparse_states * 4U) + 16U;

    while (state != P4_IDX_NONE) {
        const struct p4_parse_state *st = &prog->parse_states[state];
        uint8_t key[P4_MAX_KEY_BYTES];
        uint32_t key_bytes = 0;
        p4_idx_t next = P4_IDX_NONE;
        bool matched = false;

        if (++steps > max_steps) {
            return -ELOOP;
        }

        for (uint16_t i = 0; i < st->nops; i++) {
            p4_idx_t h = st->ops[i].header;
            uint16_t n = hdr_bytes(prog, h);

            /* The frame has to actually hold what the program extracts. */
            if (n > len - pos) {
                return -EMSGSIZE;
            }
            memcpy(phv->data + prog->headers[h].phv_offset, frame + pos, n);
            phv->valid[h] = true;
            pos += n;
        }

        if (st->nkey == 0) {
            /* No select: the one arm, if any, is taken unconditionally. */
            if (st->ntransitions > 0) {
                next = st->transitions[0].next_state;
            }
            state = next;
            continue;
        }

        /* Build the select key by concatenating its fields, each
         * right-aligned in whole bytes of its own: the layout the loader
         * sized it for and the arms' values are written in. */
        memset(key, 0, sizeof(key));
        for (uint16_t i = 0; i < st->nkey; i++) {
            const struct p4_field *f = field_of(prog, st->key[i]);
            struct p4_value v;

            key_bytes += (f->bits + 7U) / 8U;
            phv_get(prog, phv, st->key[i], &v);
            bits_set(key, (key_bytes * 8U) - f->bits, f->bits, &v);
        }

        for (uint16_t i = 0; i < st->ntransitions && !matched; i++) {
            const struct p4_transition *t = &st->transitions[i];

            if (t->is_default) {
                next = t->next_state;
                matched = true;
                break;
            }
            /* The arm's value is right-aligned in its buffer, as is the
             * key, so both are compared over the key's width. */
            bool eq = true;
            for (uint16_t b = 0; b < st->key_bytes && eq; b++) {
                uint16_t ko = (uint16_t)(P4_MAX_KEY_BYTES - st->key_bytes + b);
                uint8_t kb = key[b];
                uint8_t vb = t->value[ko];

                if (t->has_mask) {
                    kb &= t->mask[ko];
                    vb &= t->mask[ko];
                }
                if (kb != vb) {
                    eq = false;
                }
            }
            if (eq) {
                next = t->next_state;
                matched = true;
            }
        }
        /*
         * No arm matched and no default.  P4 calls that error.NoMatch and
         * rejects; bmv2 has no reject state and would end the parse as if
         * it had accepted.  Here it is a parse error, and the frame is
         * dropped.
         */
        if (!matched) {
            return -EBADMSG;
        }
        state = next;
    }
    *consumed = pos;
    return 0;
}

/* ---- actions ----------------------------------------------------------- */

struct p4_args {
    uint16_t n;
    const uint8_t *val[P4_MAX_RUNTIME_ARGS];
    uint16_t len[P4_MAX_RUNTIME_ARGS];
};

static void operand_value(const struct p4_program *prog,
                          const struct p4_phv *phv, const struct p4_operand *op,
                          const struct p4_args *args, struct p4_value *out)
{
    switch (op->kind) {
    case P4_OPD_FIELD:
        phv_get(prog, phv, op->field, out);
        return;
    case P4_OPD_HEXSTR:
        imm_value(op->imm, P4_MAX_IMM_BYTES, op->imm_len, out);
        return;
    case P4_OPD_RUNTIME:
        if (op->runtime < args->n) {
            imm_value(args->val[op->runtime], P4_MAX_IMM_BYTES,
                      args->len[op->runtime], out);
            return;
        }
        value_zero(out);
        return;
    default:
        value_zero(out);
        return;
    }
}

/*
 * Run one action's primitives.
 *
 * Returns 0, or a negative errno when a primitive could not be carried
 * out.  The built-in primitives are total; an extern call is not.  It
 * reaches outside the pipeline, can decline or fail there, and fails
 * outright on the frame path, which has no regions to give it.  Its error
 * has to reach the caller rather than leaving the operation silently not
 * done while the work request that asked for it completes successfully.
 */
static int run_action(const struct p4_program *prog, struct p4_phv *phv,
                      const struct p4_extern_ops *ops,
                      const struct p4_action *act, const struct p4_args *args,
                      bool *dropped)
{
    for (uint16_t i = 0; i < act->nprimitives; i++) {
        const struct p4_primitive *p = &act->primitives[i];
        struct p4_value a;
        struct p4_value b;
        struct p4_value r;

        switch (p->kind) {
        case P4_PRIM_DROP:
            *dropped = true;
            break;
        case P4_PRIM_NOP:
            break;
        case P4_PRIM_ASSIGN:
            operand_value(prog, phv, &p->params[1], args, &b);
            phv_set(prog, phv, p->params[0].field, &b);
            break;
        case P4_PRIM_ADD:
        case P4_PRIM_SUB:
            /* Two parameters means the destination is also the first
             * operand, which is what add_to_field spells. */
            if (p->nparams == 2) {
                phv_get(prog, phv, p->params[0].field, &a);
                operand_value(prog, phv, &p->params[1], args, &b);
            } else {
                operand_value(prog, phv, &p->params[1], args, &a);
                operand_value(prog, phv, &p->params[2], args, &b);
            }
            /* Wrap-around is what a fixed-width field does, and phv_set
             * truncates the result to the destination's width. */
            if (p->kind == P4_PRIM_ADD) {
                value_add(&a, &b, &r);
            } else {
                value_sub(&a, &b, &r);
            }
            phv_set(prog, phv, p->params[0].field, &r);
            break;
        case P4_PRIM_EXTERN: {
            struct p4_value argv[P4_MAX_PRIM_PARAMS];
            uint8_t argc = 0;
            int rc;

            /*
             * No operands to work on. The frame path passes no ops at
             * all, because an extern's regions come from a work request
             * and a frame has none; failing is the only honest answer,
             * since carrying on would run part of an action.
             */
            if (!ops || !ops->call) {
                return -EPERM;
            }

            /* params[0] is the instance; the rest are the arguments. */
            for (uint8_t q = 1; q < p->nparams; q++) {
                operand_value(prog, phv, &p->params[q], args, &argv[argc++]);
            }

            rc = ops->call(ops->ctx, prog->externs[p->params[0].inst].type,
                           p->method, argv, argc);
            if (rc) {
                return rc;
            }
            break;
        }
        default:
            /* The loader accepts only the primitives above, so reaching
             * here means the program and the executor disagree about what
             * was loaded.  Failing the packet is the only honest answer;
             * carrying on would run some of an action and not the rest. */
            return -ENOSYS;
        }
    }
    return 0;
}

/* ---- conditionals ------------------------------------------------------ */

static bool eval_node(const struct p4_program *prog, const struct p4_phv *phv,
                      const struct p4_conditional *c, p4_idx_t idx,
                      unsigned depth);

/* Recursion through eval_node() is bounded by P4_MAX_EXPR_DEPTH. */
/* NOLINTNEXTLINE(misc-no-recursion) */
static void eval_operand(const struct p4_program *prog,
                         const struct p4_phv *phv,
                         const struct p4_conditional *c,
                         const struct p4_expr_operand *o, unsigned depth,
                         struct p4_value *out)
{
    switch (o->kind) {
    case P4_EO_FIELD:
        phv_get(prog, phv, o->field, out);
        return;
    case P4_EO_HEXSTR:
        imm_value(o->imm, P4_MAX_IMM_BYTES, o->imm_len, out);
        return;
    case P4_EO_HEADER:
        /* Only valid() takes a header, and it reads the validity bit. */
        value_set_u32(out, phv->valid[o->header] ? 1U : 0U);
        return;
    case P4_EO_BOOL:
        value_set_u32(out, o->bval ? 1U : 0U);
        return;
    case P4_EO_EXPR:
        value_set_u32(out,
                      eval_node(prog, phv, c, o->expr, depth + 1) ? 1U : 0U);
        return;
    default:
        value_zero(out);
        return;
    }
}

/* Recursion through eval_operand() is bounded by P4_MAX_EXPR_DEPTH. */
/* NOLINTNEXTLINE(misc-no-recursion) */
static bool eval_node(const struct p4_program *prog, const struct p4_phv *phv,
                      const struct p4_conditional *c, p4_idx_t idx,
                      unsigned depth)
{
    const struct p4_expr_node *n;
    struct p4_value l;
    struct p4_value r;
    int cmp;

    /* A child's index is always greater than its parent's, so this cannot
     * recurse forever; the bound is belt and braces against a later edit. */
    if (depth > P4_MAX_EXPR_DEPTH || idx >= c->nexpr) {
        return false;
    }
    n = &c->expr[idx];

    if (n->op == P4_EXPR_VALID || n->op == P4_EXPR_NOT) {
        eval_operand(prog, phv, c, &n->right, depth, &r);
        return (n->op == P4_EXPR_VALID) ? value_nonzero(&r)
                                        : !value_nonzero(&r);
    }

    eval_operand(prog, phv, c, &n->left, depth, &l);
    eval_operand(prog, phv, c, &n->right, depth, &r);
    /* Both are right-aligned in the same width, so one memcmp orders
     * them whatever the fields' widths are. */
    cmp = value_cmp(&l, &r);

    switch (n->op) {
    case P4_EXPR_EQ:
        return cmp == 0;
    case P4_EXPR_NE:
        return cmp != 0;
    case P4_EXPR_LT:
        return cmp < 0;
    case P4_EXPR_LE:
        return cmp <= 0;
    case P4_EXPR_GT:
        return cmp > 0;
    case P4_EXPR_GE:
        return cmp >= 0;
    case P4_EXPR_AND:
        return value_nonzero(&l) && value_nonzero(&r);
    case P4_EXPR_OR:
        return value_nonzero(&l) || value_nonzero(&r);
    default:
        return false;
    }
}

/* ---- the pipeline ------------------------------------------------------ */

static int run_pipeline(const struct p4_program *prog, struct p4_phv *phv,
                        const struct p4_entries *ent,
                        const struct p4_extern_ops *ops, p4_idx_t pipe,
                        const struct p4_pipeline *pl, bool *dropped)
{
    p4_idx_t node = pl->init_node;
    unsigned steps = 0;
    int rc;
    /* The graph was shown acyclic at load; this bounds it anyway, because
     * the thread running it also services the vfio-user poll loop. */
    const unsigned max_steps = (unsigned)pl->nnodes + 1U;

    while (node != P4_IDX_NONE) {
        const struct p4_node *n = &pl->nodes[node];

        if (++steps > max_steps) {
            return -ELOOP;
        }

        if (n->kind == P4_NODE_CONDITIONAL) {
            const struct p4_conditional *c = &pl->conditionals[n->index];

            node = eval_node(prog, phv, c, 0, 0) ? c->true_next : c->false_next;
            continue;
        }

        const struct p4_table *t = &pl->tables[n->index];
        struct p4_value keys[P4_MAX_TABLE_KEY];
        const struct p4_entry *hit = NULL;
        struct p4_args args;

        for (uint16_t i = 0; i < t->nkey; i++) {
            phv_get(prog, phv, t->key[i].field, &keys[i]);
        }
        hit = p4_entries_lookup(ent, prog, pipe, n->index, keys);

        memset(&args, 0, sizeof(args));
        if (hit) {
            args.n = hit->nparams;
            for (uint16_t i = 0; i < hit->nparams; i++) {
                args.val[i] = hit->params[i].value;
                args.len[i] = hit->params[i].len;
            }
            rc = run_action(prog, phv, ops, &prog->actions[hit->action], &args,
                            dropped);
            if (rc) {
                return rc;
            }
            /* A hit follows the edge for the action that matched. */
            node = P4_IDX_NONE;
            for (uint16_t i = 0; i < t->nactions; i++) {
                if (t->actions[i] == hit->action) {
                    node = t->next[i];
                    break;
                }
            }
            continue;
        }

        /* A miss runs the default action, if the program named one, and
         * takes the miss edge. */
        if (t->default_action != P4_IDX_NONE) {
            args.n = t->ndefault_data;
            for (uint16_t i = 0; i < t->ndefault_data; i++) {
                args.val[i] = t->default_data[i];
                args.len[i] = t->default_data_len[i];
            }
            rc = run_action(prog, phv, ops, &prog->actions[t->default_action],
                            &args, dropped);
            if (rc) {
                return rc;
            }
        }
        node = t->base_default_next;
    }
    return 0;
}

/* ---- deparsing --------------------------------------------------------- */

static int run_deparser(const struct p4_program *prog, const struct p4_phv *phv,
                        const uint8_t *frame, size_t len, size_t consumed,
                        uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t pos = 0;

    for (uint16_t i = 0; i < prog->ndeparse_order; i++) {
        p4_idx_t h = prog->deparse_order[i];
        uint16_t n;

        if (!phv->valid[h]) {
            continue;
        }
        n = hdr_bytes(prog, h);
        if (n > out_cap - pos) {
            return -EMSGSIZE;
        }
        memcpy(out + pos, phv->data + prog->headers[h].phv_offset, n);
        pos += n;
    }

    /* Whatever the parser did not consume is the payload, carried through
     * untouched. */
    if (len - consumed > out_cap - pos) {
        return -EMSGSIZE;
    }
    memcpy(out + pos, frame + consumed, len - consumed);
    pos += len - consumed;

    *out_len = pos;
    return 0;
}

/*
 * Write an architecture field, if the program declared it.
 *
 * The guard is not a nicety: phv_set() indexes prog->headers[ref.header]
 * without checking, so a write through a reference left at P4_IDX_NONE
 * would read far past the end of that array.
 */
static void meta_set(const struct p4_program *prog, struct p4_phv *phv,
                     struct p4_field_ref ref, uint32_t v)
{
    struct p4_value val;

    if (ref.header == P4_IDX_NONE) {
        return;
    }
    value_set_u32(&val, v);
    phv_set(prog, phv, ref, &val);
}

/* egress_spec, which every program declares; the loader refuses one that
 * does not. */
static uint32_t egress_spec_get(const struct p4_program *prog,
                                const struct p4_phv *phv)
{
    struct p4_value v;

    phv_get(prog, phv, prog->egress_spec, &v);
    return value_u32(&v);
}

/*
 * Hand a packet from ingress to egress, as simple_switch does: egress_port
 * says where ingress sent it, @port, and egress_spec starts again at one
 * past P4_DROP_PORT -- 512, which v1model's nine bits hold as 0 -- so that
 * egress drops it only by assigning the drop port itself.  From here on
 * @phv->egress_assigned is about egress's assignments, not ingress's.
 */
static void enter_egress(const struct p4_program *prog, struct p4_phv *phv,
                         uint32_t port)
{
    meta_set(prog, phv, prog->egress_port, port);
    meta_set(prog, phv, prog->egress_spec, P4_DROP_PORT + 1U);
    phv->egress_assigned = false;
}

int p4_exec_run_meta(const struct p4_program *prog,
                     const struct p4_entries *ent,
                     const struct p4_extern_ops *ops,
                     const struct p4_rdma_meta *meta,
                     struct p4_exec_result *res)
{
    struct p4_phv phv;
    bool dropped = false;
    int rc;

    if (!prog || !meta || !res) {
        return -EINVAL;
    }
    memset(res, 0, sizeof(*res));
    if (!prog->has_rdma_metadata) {
        return -ENOENT;
    }

    rc = phv_init(prog, &phv);
    if (rc) {
        return rc;
    }

    /* Metadata is not on a wire, so it is valid from the start. Every
     * other header stays invalid, which is what makes valid() false and
     * field reads zero for a program that also serves Ethernet. */
    for (uint16_t h = 0; h < prog->nheaders; h++) {
        if (prog->headers[h].metadata) {
            phv.valid[h] = true;
        }
    }

    meta_set(prog, &phv, prog->ingress_port, P4_PORT_RDMA);
    meta_set(prog, &phv, prog->rdma_opcode, meta->opcode);
    meta_set(prog, &phv, prog->rdma_qp_num, meta->qp_num);
    meta_set(prog, &phv, prog->rdma_length, meta->length);
    meta_set(prog, &phv, prog->rdma_imm_data, meta->imm_data);
    meta_set(prog, &phv, prog->rdma_rkey, meta->rkey);

    rc = run_pipeline(prog, &phv, ent, ops, P4_PIPE_INGRESS,
                      &prog->pipelines[P4_PIPE_INGRESS], &dropped);
    /* An operation dropped in ingress never reaches egress, as in
     * simple_switch, so egress's externs are not called for it. */
    if (rc == 0 && !dropped) {
        /* egress_spec decides nothing here (see the header), but egress
         * still sees it as egress_port, as simple_switch would show it. */
        enter_egress(prog, &phv, egress_spec_get(prog, &phv));
        rc = run_pipeline(prog, &phv, ent, ops, P4_PIPE_EGRESS,
                          &prog->pipelines[P4_PIPE_EGRESS], &dropped);
    }
    if (rc) {
        /* A drop marked before the pipeline stopped still stands; see the
         * header. */
        res->dropped = dropped;
        return rc;
    }

    /* egress_spec is deliberately not read for the verdict; see the
     * header. */
    res->dropped = dropped;
    res->len = 0;
    return 0;
}

int p4_exec_run(const struct p4_program *prog, const struct p4_entries *ent,
                uint32_t ingress_port, uint32_t default_port, const void *frame,
                size_t len, void *out, size_t out_cap,
                struct p4_exec_result *res)
{
    struct p4_phv phv;
    bool dropped = false;
    bool assigned;
    uint32_t port;
    size_t consumed = 0;
    int rc;

    if (!prog || !frame || !out || !res || len == 0) {
        return -EINVAL;
    }

    memset(res, 0, sizeof(*res));

    rc = phv_init(prog, &phv);
    if (rc) {
        return rc;
    }

    /* Metadata is not on the wire, so it is valid from the start. */
    for (uint16_t h = 0; h < prog->nheaders; h++) {
        if (prog->headers[h].metadata) {
            phv.valid[h] = true;
        }
    }
    if (prog->ingress_port.header != P4_IDX_NONE) {
        struct p4_value v;

        value_set_u32(&v, ingress_port);
        phv_set(prog, &phv, prog->ingress_port, &v);
    }

    rc = run_parser(prog, &phv, frame, len, &consumed);
    if (rc) {
        return rc;
    }

    /* No ops on this path: an extern's operands come from a work request,
     * and a frame has none.  See run_action(). */
    rc = run_pipeline(prog, &phv, ent, NULL, P4_PIPE_INGRESS,
                      &prog->pipelines[P4_PIPE_INGRESS], &dropped);
    if (rc) {
        return rc;
    }

    /* Ingress decides where the frame goes; egress may drop it but not
     * send it elsewhere, as in simple_switch. */
    assigned = phv.egress_assigned;
    port = egress_spec_get(prog, &phv);
    if (port == P4_DROP_PORT) {
        dropped = true;
    }

    /* A frame dropped in ingress never reaches egress. */
    if (!dropped) {
        enter_egress(prog, &phv, assigned ? port : default_port);
        rc = run_pipeline(prog, &phv, ent, NULL, P4_PIPE_EGRESS,
                          &prog->pipelines[P4_PIPE_EGRESS], &dropped);
        if (rc) {
            return rc;
        }
        if (egress_spec_get(prog, &phv) == P4_DROP_PORT) {
            dropped = true;
        }
    }

    res->dropped = dropped;
    res->egress_assigned = assigned;
    res->egress_spec = port;
    if (dropped) {
        return 0;
    }

    return run_deparser(prog, &phv, frame, len, consumed, out, out_cap,
                        &res->len);
}
