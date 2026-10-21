/*
 * p4_exec.c — the pipeline executor
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <string.h>

#include "p4_exec.h"
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

/* Read @bits starting at @bit_off of @src, big-endian, as a value. */
static uint64_t bits_get(const uint8_t *src, uint32_t bit_off, uint16_t bits)
{
    uint64_t v = 0;

    for (uint16_t i = 0; i < bits; i++) {
        uint32_t b = bit_off + i;

        v = (v << 1) | (((uint32_t)src[b / 8u] >> (7u - (b % 8u))) & 1u);
    }
    return v;
}

/* Write the low @bits of @v at @bit_off of @dst, big-endian. */
static void bits_set(uint8_t *dst, uint32_t bit_off, uint16_t bits, uint64_t v)
{
    for (uint16_t i = 0; i < bits; i++) {
        uint32_t b = bit_off + i;
        const uint8_t mask = (uint8_t)(1u << (7u - (b % 8u)));
        uint64_t bit = (v >> (bits - 1u - i)) & 1u;

        if (bit)
            dst[b / 8u] |= mask;
        else
            dst[b / 8u] &= (uint8_t)~mask;
    }
}

static const struct p4_field *field_of(const struct p4_program *prog,
                                       struct p4_field_ref ref)
{
    return &prog->header_types[prog->headers[ref.header].header_type]
                .fields[ref.field];
}

static uint64_t phv_get(const struct p4_program *prog, const struct p4_phv *phv,
                        struct p4_field_ref ref)
{
    const struct p4_field *f = field_of(prog, ref);

    if (f->bits > P4_FIELD_VALUE_BITS)
        return 0;
    return bits_get(phv->data,
                    (uint32_t)phv->offset[ref.header] * 8u + f->offset_bits,
                    f->bits);
}

static void phv_set(const struct p4_program *prog, struct p4_phv *phv,
                    struct p4_field_ref ref, uint64_t v)
{
    const struct p4_field *f = field_of(prog, ref);

    if (f->bits > P4_FIELD_VALUE_BITS)
        return;
    bits_set(phv->data, (uint32_t)phv->offset[ref.header] * 8u + f->offset_bits,
             f->bits, v);
}

/*
 * A literal from the program, as a value.
 *
 * ernic_parse_hexstr() stores a value right-aligned in the whole buffer it
 * was given and reports separately how many bytes the digits needed, so
 * the significant bytes are the LAST @len of @cap -- not the first. Reading
 * from the front yields zero for every literal narrower than the buffer.
 */
static uint64_t imm_value(const uint8_t *imm, uint16_t cap, uint16_t len)
{
    uint64_t v = 0;
    uint16_t n = (len > 8u) ? 8u : len;

    if (n > cap)
        n = cap;
    for (uint16_t i = (uint16_t)(cap - n); i < cap; i++)
        v = (v << 8) | imm[i];
    return v;
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

        /* Build the select key by concatenating its fields. */
        memset(key, 0, sizeof(key));
        for (uint16_t i = 0; i < st->nkey; i++) {
            const struct p4_field *f = field_of(prog, st->key[i]);
            uint64_t v = phv_get(prog, phv, st->key[i]);

            if (f->bits > P4_FIELD_VALUE_BITS)
                return -ENOTSUP;
            bits_set(key, key_bits, f->bits, v);
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

static uint64_t operand_value(const struct p4_program *prog,
                              const struct p4_phv *phv,
                              const struct p4_operand *op,
                              const struct p4_args *args)
{
    switch (op->kind) {
    case P4_OPD_FIELD:
        return phv_get(prog, phv, op->field);
    case P4_OPD_HEXSTR:
        return imm_value(op->imm, P4_MAX_IMM_BYTES, op->imm_len);
    case P4_OPD_RUNTIME:
        if (op->runtime < args->n)
            return imm_value(args->val[op->runtime], P4_MAX_IMM_BYTES,
                             args->len[op->runtime]);
        return 0;
    default:
        return 0;
    }
}

static void run_action(const struct p4_program *prog, struct p4_phv *phv,
                       const struct p4_action *act, const struct p4_args *args,
                       bool *dropped)
{
    for (uint16_t i = 0; i < act->nprimitives; i++) {
        const struct p4_primitive *p = &act->primitives[i];
        uint64_t a, b;

        switch (p->kind) {
        case P4_PRIM_DROP:
            *dropped = true;
            break;
        case P4_PRIM_NOP:
            break;
        case P4_PRIM_ASSIGN:
            phv_set(prog, phv, p->params[0].field,
                    operand_value(prog, phv, &p->params[1], args));
            break;
        case P4_PRIM_ADD:
        case P4_PRIM_SUB:
            /* Two parameters means the destination is also the first
             * operand, which is what add_to_field spells. */
            if (p->nparams == 2) {
                a = phv_get(prog, phv, p->params[0].field);
                b = operand_value(prog, phv, &p->params[1], args);
            } else {
                a = operand_value(prog, phv, &p->params[1], args);
                b = operand_value(prog, phv, &p->params[2], args);
            }
            /* Unsigned wrap-around is what a fixed-width field does, and
             * phv_set truncates to the field's width. */
            phv_set(prog, phv, p->params[0].field,
                    (p->kind == P4_PRIM_ADD) ? a + b : a - b);
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

static uint64_t eval_operand(const struct p4_program *prog,
                             const struct p4_phv *phv,
                             const struct p4_conditional *c,
                             const struct p4_expr_operand *o, unsigned depth,
                             bool *as_bool)
{
    *as_bool = false;
    switch (o->kind) {
    case P4_EO_FIELD:
        return phv_get(prog, phv, o->field);
    case P4_EO_HEXSTR:
        return imm_value(o->imm, P4_MAX_IMM_BYTES, o->imm_len);
    case P4_EO_HEADER:
        /* Only valid() takes a header, and it reads the validity bit. */
        *as_bool = true;
        return phv->valid[o->header] ? 1u : 0u;
    case P4_EO_BOOL:
        *as_bool = true;
        return o->bval ? 1u : 0u;
    case P4_EO_EXPR:
        *as_bool = true;
        return eval_node(prog, phv, c, o->expr, depth + 1) ? 1u : 0u;
    default:
        return 0;
    }
}

static bool eval_node(const struct p4_program *prog, const struct p4_phv *phv,
                      const struct p4_conditional *c, p4_idx_t idx,
                      unsigned depth)
{
    const struct p4_expr_node *n;
    uint64_t l, r;
    bool lb, rb;

    /* A child's index is always greater than its parent's, so this cannot
     * recurse forever; the bound is belt and braces against a later edit. */
    if (depth > P4_MAX_EXPR_DEPTH || idx >= c->nexpr)
        return false;
    n = &c->expr[idx];

    if (n->op == P4_EXPR_VALID || n->op == P4_EXPR_NOT) {
        r = eval_operand(prog, phv, c, &n->right, depth, &rb);
        (void)rb;
        return (n->op == P4_EXPR_VALID) ? (r != 0) : (r == 0);
    }

    l = eval_operand(prog, phv, c, &n->left, depth, &lb);
    r = eval_operand(prog, phv, c, &n->right, depth, &rb);

    switch (n->op) {
    case P4_EXPR_EQ:
        return l == r;
    case P4_EXPR_NE:
        return l != r;
    case P4_EXPR_LT:
        return l < r;
    case P4_EXPR_LE:
        return l <= r;
    case P4_EXPR_GT:
        return l > r;
    case P4_EXPR_GE:
        return l >= r;
    case P4_EXPR_AND:
        return (l != 0) && (r != 0);
    case P4_EXPR_OR:
        return (l != 0) || (r != 0);
    default:
        return false;
    }
}

/* ---- the pipeline ------------------------------------------------------ */

static int run_pipeline(const struct p4_program *prog, struct p4_phv *phv,
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

        /*
         * Every lookup misses: there is no control plane, so no table has
         * entries. A miss runs the default action, if the program named
         * one, and goes to the miss target.
         */
        if (t->default_action != P4_IDX_NONE) {
            struct p4_args args;

            memset(&args, 0, sizeof(args));
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

int p4_exec_run(const struct p4_program *prog, uint32_t ingress_port,
                const void *frame, size_t len, void *out, size_t out_cap,
                struct p4_exec_result *res)
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
    if (prog->ingress_port.header != P4_IDX_NONE)
        phv_set(prog, &phv, prog->ingress_port, ingress_port);

    rc = run_parser(prog, &phv, frame, len);
    if (rc)
        return rc;

    for (uint16_t p = 0; p < prog->npipelines; p++) {
        rc = run_pipeline(prog, &phv, &prog->pipelines[p], &dropped);
        if (rc)
            return rc;
    }

    res->dropped = dropped;
    res->egress_spec = (uint32_t)phv_get(prog, &phv, prog->egress_spec);
    if (dropped)
        return 0;

    return run_deparser(prog, &phv, frame, len, parsed_bytes(prog, &phv), out,
                        out_cap, &res->len);
}
