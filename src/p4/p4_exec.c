/*
 * p4_exec.c — the pipeline executor
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <string.h>

#include "p4_exec.h"
#include "p4_table.h"
#include "p4_value.h"
#include "p4_program.h"

/*
 * The packet header vector: one contiguous byte range per header instance,
 * laid out in the order the headers were declared, plus a validity bit
 * each.  Fields are addressed by bit offset within their header, exactly
 * as they arrived on the wire, so extraction is a copy and deparsing is
 * the same copy back.
 */
struct p4_phv {
    uint8_t data[P4_PHV_BYTES];
    uint16_t offset[P4_MAX_HEADERS]; /* byte offset of each header */
    bool valid[P4_MAX_HEADERS];
};

/* Bytes a header occupies, rounded up from its width in bits. */
static uint16_t hdr_bytes(const struct p4_program *prog, p4_idx_t h)
{
    return (
        uint16_t)((prog->header_types[prog->headers[h].header_type].total_bits +
                   7u) /
                  8u);
}

/*
 * Lay the headers out. Done per packet rather than once at load because
 * the layout belongs to the packet, not the program, and a program is
 * shared across the threads that may eventually run it.
 */
static int phv_init(const struct p4_program *prog, struct p4_phv *phv)
{
    uint32_t off = 0;

    memset(phv, 0, sizeof(*phv));
    for (uint16_t h = 0; h < prog->nheaders; h++) {
        uint16_t n = hdr_bytes(prog, h);

        /* Subtraction first: the sum must not be formed and then checked. */
        if (n > (uint32_t)P4_PHV_BYTES - off)
            return -ENOSPC;
        phv->offset[h] = (uint16_t)off;
        off += n;
    }
    return 0;
}

static const struct p4_field *field_of(const struct p4_program *prog,
                                       struct p4_field_ref ref)
{
    return &prog->header_types[prog->headers[ref.header].header_type]
                .fields[ref.field];
}

static void phv_get(const struct p4_program *prog, const struct p4_phv *phv,
                    struct p4_field_ref ref, struct p4_value *out)
{
    const struct p4_field *f = field_of(prog, ref);

    bits_get(phv->data, (uint32_t)phv->offset[ref.header] * 8u + f->offset_bits,
             f->bits, out);
}

static void phv_set(const struct p4_program *prog, struct p4_phv *phv,
                    struct p4_field_ref ref, const struct p4_value *v)
{
    const struct p4_field *f = field_of(prog, ref);

    bits_set(phv->data, (uint32_t)phv->offset[ref.header] * 8u + f->offset_bits,
             f->bits, v);
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
    if (n > P4_VALUE_BYTES)
        n = P4_VALUE_BYTES;
    for (uint16_t i = 0; i < n; i++)
        out->b[P4_VALUE_BYTES - 1u - i] = imm[cap - 1u - i];
}

/* ---- the parser -------------------------------------------------------- */

static int run_parser(const struct p4_program *prog, struct p4_phv *phv,
                      const uint8_t *frame, size_t len)
{
    p4_idx_t state = prog->init_state;
    size_t pos = 0;
    /* A parse state graph may legitimately revisit a state, so progress is
     * bounded by steps rather than by a visited set. */
    unsigned steps = 0;
    const unsigned max_steps = (unsigned)prog->nparse_states * 4u + 16u;

    while (state != P4_IDX_NONE) {
        const struct p4_parse_state *st = &prog->parse_states[state];
        uint8_t key[P4_MAX_KEY_BYTES];
        uint32_t key_bits = 0;
        p4_idx_t next = P4_IDX_NONE;
        bool matched = false;

        if (++steps > max_steps)
            return -ELOOP;

        for (uint16_t i = 0; i < st->nops; i++) {
            p4_idx_t h = st->ops[i].header;
            uint16_t n = hdr_bytes(prog, h);

            /* The frame has to actually hold what the program extracts. */
            if (n > len - pos)
                return -EMSGSIZE;
            memcpy(phv->data + phv->offset[h], frame + pos, n);
            phv->valid[h] = true;
            pos += n;
        }

        if (st->nkey == 0) {
            /* No select: the one arm, if any, is taken unconditionally. */
            if (st->ntransitions > 0)
                next = st->transitions[0].next_state;
            state = next;
            continue;
        }

        /* Build the select key by concatenating its fields. Any width
         * works: a value is the same shape whatever the field. */
        memset(key, 0, sizeof(key));
        for (uint16_t i = 0; i < st->nkey; i++) {
            const struct p4_field *f = field_of(prog, st->key[i]);
            struct p4_value v;

            phv_get(prog, phv, st->key[i], &v);
            bits_set(key, key_bits, f->bits, &v);
            key_bits += f->bits;
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
                if (kb != vb)
                    eq = false;
            }
            if (eq) {
                next = t->next_state;
                matched = true;
            }
        }
        /* No arm matched and no default: bmv2 rejects the packet. */
        if (!matched)
            return -EBADMSG;
        state = next;
    }
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

static void run_action(const struct p4_program *prog, struct p4_phv *phv,
                       const struct p4_action *act, const struct p4_args *args,
                       bool *dropped)
{
    for (uint16_t i = 0; i < act->nprimitives; i++) {
        const struct p4_primitive *p = &act->primitives[i];
        struct p4_value a, b, r;

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
            if (p->kind == P4_PRIM_ADD)
                value_add(&a, &b, &r);
            else
                value_sub(&a, &b, &r);
            phv_set(prog, phv, p->params[0].field, &r);
            break;
        default:
            break;
        }
    }
}

/* ---- conditionals ------------------------------------------------------ */

static bool eval_node(const struct p4_program *prog, const struct p4_phv *phv,
                      const struct p4_conditional *c, p4_idx_t idx,
                      unsigned depth);

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
        value_set_u32(out, phv->valid[o->header] ? 1u : 0u);
        return;
    case P4_EO_BOOL:
        value_set_u32(out, o->bval ? 1u : 0u);
        return;
    case P4_EO_EXPR:
        value_set_u32(out,
                      eval_node(prog, phv, c, o->expr, depth + 1) ? 1u : 0u);
        return;
    default:
        value_zero(out);
        return;
    }
}

static bool eval_node(const struct p4_program *prog, const struct p4_phv *phv,
                      const struct p4_conditional *c, p4_idx_t idx,
                      unsigned depth)
{
    const struct p4_expr_node *n;
    struct p4_value l, r;
    int cmp;

    /* A child's index is always greater than its parent's, so this cannot
     * recurse forever; the bound is belt and braces against a later edit. */
    if (depth > P4_MAX_EXPR_DEPTH || idx >= c->nexpr)
        return false;
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
                        const struct p4_entries *ent, p4_idx_t pipe,
                        const struct p4_pipeline *pl, bool *dropped)
{
    p4_idx_t node = pl->init_node;
    unsigned steps = 0;
    /* The graph was shown acyclic at load; this bounds it anyway, because
     * the thread running it also services the vfio-user poll loop. */
    const unsigned max_steps = (unsigned)pl->nnodes + 1u;

    while (node != P4_IDX_NONE) {
        const struct p4_node *n = &pl->nodes[node];

        if (++steps > max_steps)
            return -ELOOP;

        if (n->kind == P4_NODE_CONDITIONAL) {
            const struct p4_conditional *c = &pl->conditionals[n->index];

            node = eval_node(prog, phv, c, 0, 0) ? c->true_next : c->false_next;
            continue;
        }

        const struct p4_table *t = &pl->tables[n->index];
        struct p4_value keys[P4_MAX_TABLE_KEY];
        const struct p4_entry *hit = NULL;
        struct p4_args args;

        for (uint16_t i = 0; i < t->nkey; i++)
            phv_get(prog, phv, t->key[i].field, &keys[i]);
        hit = p4_entries_lookup(ent, prog, pipe, n->index, keys);

        memset(&args, 0, sizeof(args));
        if (hit) {
            args.n = hit->nparams;
            for (uint16_t i = 0; i < hit->nparams; i++) {
                args.val[i] = hit->params[i].value;
                args.len[i] = hit->params[i].len;
            }
            run_action(prog, phv, &prog->actions[hit->action], &args, dropped);
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
            run_action(prog, phv, &prog->actions[t->default_action], &args,
                       dropped);
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

        if (!phv->valid[h])
            continue;
        n = hdr_bytes(prog, h);
        if (n > out_cap - pos)
            return -EMSGSIZE;
        memcpy(out + pos, phv->data + phv->offset[h], n);
        pos += n;
    }

    /* Whatever the parser did not consume is the payload, carried through
     * untouched. */
    if (len - consumed > out_cap - pos)
        return -EMSGSIZE;
    memcpy(out + pos, frame + consumed, len - consumed);
    pos += len - consumed;

    *out_len = pos;
    return 0;
}

/*
 * How many bytes the parser took, which is what the deparser must skip.
 * Derived from the headers it marked valid rather than tracked separately,
 * so the two cannot disagree.
 */
static size_t parsed_bytes(const struct p4_program *prog,
                           const struct p4_phv *phv)
{
    size_t n = 0;

    for (uint16_t h = 0; h < prog->nheaders; h++) {
        if (phv->valid[h] && !prog->headers[h].metadata)
            n += hdr_bytes(prog, h);
    }
    return n;
}

int p4_exec_run(const struct p4_program *prog, const struct p4_entries *ent,
                uint32_t ingress_port, const void *frame, size_t len, void *out,
                size_t out_cap, struct p4_exec_result *res)
{
    struct p4_phv phv;
    bool dropped = false;
    int rc;

    if (!prog || !frame || !out || !res || len == 0)
        return -EINVAL;

    memset(res, 0, sizeof(*res));

    rc = phv_init(prog, &phv);
    if (rc)
        return rc;

    /* Metadata is not on the wire, so it is valid from the start. */
    for (uint16_t h = 0; h < prog->nheaders; h++) {
        if (prog->headers[h].metadata)
            phv.valid[h] = true;
    }
    if (prog->ingress_port.header != P4_IDX_NONE) {
        struct p4_value v;

        value_set_u32(&v, ingress_port);
        phv_set(prog, &phv, prog->ingress_port, &v);
    }

    rc = run_parser(prog, &phv, frame, len);
    if (rc)
        return rc;

    for (uint16_t p = 0; p < prog->npipelines; p++) {
        rc = run_pipeline(prog, &phv, ent, (p4_idx_t)p, &prog->pipelines[p],
                          &dropped);
        if (rc)
            return rc;
    }

    res->dropped = dropped;
    {
        struct p4_value v;

        phv_get(prog, &phv, prog->egress_spec, &v);
        res->egress_spec = value_u32(&v);
    }
    if (dropped)
        return 0;

    return run_deparser(prog, &phv, frame, len, parsed_bytes(prog, &phv), out,
                        out_cap, &res->len);
}
