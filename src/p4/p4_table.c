/*
 * p4_table.c — the entry store and the match units
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p4/p4_table.h"
#include "p4/p4_value.h"

/* A key field's bytes are the low P4_MATCH_BYTES of a value. */
_Static_assert(P4_MATCH_BYTES <= P4_VALUE_BYTES,
               "a key field must fit in a value");

/*
 * The installed entries are kept packed at the front of @e, in no order,
 * so a lookup reads only those, however large the table: a delete moves
 * the last entry into the hole it leaves.
 */
struct p4_table_entries {
    uint32_t n;   /* entries installed, e[0] to e[n - 1] */
    uint32_t cap; /* allocated, 0 until the first insert */
    struct p4_entry *e;
};

struct p4_entries {
    struct p4_table_entries t[P4_MAX_PIPELINES][P4_MAX_TABLES];
};

static int fail(char *err, size_t err_len, int rc, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int fail(char *err, size_t err_len, int rc, const char *fmt, ...)
{
    va_list ap;

    if (err && err_len) {
        va_start(ap, fmt);
        (void)vsnprintf(err, err_len, fmt, ap);
        va_end(ap);
    }
    return rc;
}

struct p4_entries *p4_entries_create(const struct p4_program *prog)
{
    if (!prog)
        return NULL;
    return calloc(1, sizeof(struct p4_entries));
}

void p4_entries_destroy(struct p4_entries *ent)
{
    if (!ent)
        return;
    for (size_t p = 0; p < P4_MAX_PIPELINES; p++) {
        for (size_t t = 0; t < P4_MAX_TABLES; t++)
            free(ent->t[p][t].e);
    }
    free(ent);
}

/* Locate a table by name, over every pipeline. The loader refuses two
 * tables of one name, wherever they are. */
static bool find_table(const struct p4_program *prog, const char *name,
                       p4_idx_t *pipe, p4_idx_t *table)
{
    for (uint16_t p = 0; p < prog->npipelines; p++) {
        const struct p4_pipeline *pl = &prog->pipelines[p];

        for (uint16_t t = 0; t < pl->ntables; t++) {
            if (strcmp(pl->tables[t].name, name) == 0) {
                *pipe = (p4_idx_t)p;
                *table = (p4_idx_t)t;
                return true;
            }
        }
    }
    return false;
}

/*
 * Bits the significant bytes of a spec's value or mask need: the last @len
 * of P4_MATCH_BYTES, which is all the match units read.
 */
static uint32_t sig_bits(const uint8_t *v, uint16_t len)
{
    uint16_t n = (len > P4_MATCH_BYTES) ? P4_MATCH_BYTES : len;

    return be_bit_length(v + (P4_MATCH_BYTES - n), n);
}

/* A spec as a value: right-aligned, so it compares against a field. */
static void spec_value(const struct p4_match_spec *s, struct p4_value *out)
{
    uint16_t n = (s->len > P4_MATCH_BYTES) ? P4_MATCH_BYTES : s->len;

    value_zero(out);
    for (uint16_t i = 0; i < n; i++)
        out->b[P4_VALUE_BYTES - 1u - i] = s->value[P4_MATCH_BYTES - 1u - i];
}

static void spec_mask(const struct p4_match_spec *s, struct p4_value *out)
{
    uint16_t n = (s->len > P4_MATCH_BYTES) ? P4_MATCH_BYTES : s->len;

    value_zero(out);
    for (uint16_t i = 0; i < n; i++)
        out->b[P4_VALUE_BYTES - 1u - i] = s->mask[P4_MATCH_BYTES - 1u - i];
}

/* A mask with the top @bits of a @width-bit field set. */
static void prefix_mask(uint16_t width, uint16_t bits, struct p4_value *out)
{
    value_zero(out);
    for (uint16_t i = 0; i < bits && i < width; i++) {
        /* Bit i from the left of the field is (width - 1 - i) from its
         * right, and the field is right-aligned in the value. */
        uint32_t from_right = (uint32_t)(width - 1u - i);

        out->b[P4_VALUE_BYTES - 1u - (from_right / 8u)] |=
            (uint8_t)(1u << (from_right % 8u));
    }
}

/* The low P4_MATCH_BYTES of @v, which hold all of a key field. */
static void low_bytes(const struct p4_value *v, uint8_t *out)
{
    memcpy(out, &v->b[P4_VALUE_BYTES - P4_MATCH_BYTES], P4_MATCH_BYTES);
}

/* The width of the field key @i of @tb matches on. */
static uint16_t key_width(const struct p4_program *prog,
                          const struct p4_table *tb, uint16_t i)
{
    const struct p4_header_type *ht =
        &prog->header_types[prog->headers[tb->key[i].field.header].header_type];

    return ht->fields[tb->key[i].field.field].bits;
}

/*
 * @up's key in the form the lookup reads (struct p4_entry_key), worked out
 * once at install rather than for every packet.  resolve() has refused a
 * bit past an LPM prefix or outside a ternary mask, so each value is
 * already inside its mask.
 */
static void entry_keys(const struct p4_program *prog, const struct p4_table *tb,
                       const struct p4_update *up, struct p4_entry_key *out)
{
    for (uint16_t i = 0; i < tb->nkey; i++) {
        const struct p4_match_spec *s = &up->match[i];
        struct p4_value v;

        spec_value(s, &v);
        low_bytes(&v, out[i].value);
        if (tb->key[i].match_type == P4_MATCH_LPM) {
            prefix_mask(key_width(prog, tb, i), s->prefix_bits, &v);
            low_bytes(&v, out[i].mask);
        } else if (tb->key[i].match_type == P4_MATCH_TERNARY) {
            spec_mask(s, &v);
            low_bytes(&v, out[i].mask);
        } else {
            memset(out[i].mask, 0xff, sizeof(out[i].mask));
        }
    }
}

/*
 * Does @e match the packet's key, @nkey fields of P4_MATCH_BYTES each,
 * one after another in @key?
 */
static bool entry_matches(const struct p4_entry *e, uint16_t nkey,
                          const uint8_t *key)
{
    for (uint16_t k = 0; k < nkey; k++) {
        const uint8_t *field = key + (size_t)k * P4_MATCH_BYTES;

        for (size_t b = 0; b < P4_MATCH_BYTES; b++) {
            if ((field[b] & e->key[k].mask[b]) != e->key[k].value[b])
                return false;
        }
    }
    return true;
}

/* Does @v have a bit set that @m does not? */
static bool value_outside(const struct p4_value *v, const struct p4_value *m)
{
    for (size_t i = 0; i < P4_VALUE_BYTES; i++) {
        if (v->b[i] & (uint8_t)~m->b[i])
            return true;
    }
    return false;
}

/*
 * Two entries address the same row when every key field matches the same
 * packets, which, since each value is inside its mask, is when every
 * value and mask is the same.  They compare as the lookup reads them
 * (entry_keys()), the significant bytes only, rather than as they were
 * given: otherwise a byte past a spec's @len, which the lookup never
 * reads, would make a second, unreachable copy of a row.
 */
static bool same_key(const struct p4_table *tb, const struct p4_entry *e,
                     const struct p4_entry_key *keys, uint32_t priority)
{
    if (memcmp(e->key, keys, tb->nkey * sizeof(*keys)) != 0)
        return false;
    /* Two ternary rows with the same key but different priorities are
     * different rows, since both can be present and the priority picks. */
    if (tb->match_type == P4_MATCH_TERNARY && e->priority != priority)
        return false;
    return true;
}

static int resolve(const struct p4_program *prog, const struct p4_update *up,
                   p4_idx_t *pipe, p4_idx_t *table, p4_idx_t *action, char *err,
                   size_t err_len)
{
    const struct p4_table *tb;
    const struct p4_action *act;

    if (!find_table(prog, up->table, pipe, table))
        return fail(err, err_len, -EINVAL, "no table '%s'", up->table);
    tb = &prog->pipelines[*pipe].tables[*table];

    if (up->nmatch != tb->nkey)
        return fail(err, err_len, -EINVAL,
                    "table '%s' has %u key fields, the entry gives %u",
                    tb->name, tb->nkey, up->nmatch);

    /*
     * A key field, or a mask, wider than the field it matches would be
     * truncated on use, which is a silently different entry from the one
     * asked for; so would an LPM prefix longer than the field has bits.  A
     * prefix on a field that is not LPM is read by nothing, so the client
     * meant a different table.  And as P4Runtime requires, a value has no
     * bit set past its LPM prefix or outside its ternary mask, and only an
     * entry in a table with a ternary key has a priority, which is never
     * zero.  A delete names its row by the same key and priority, so it is
     * held to the same rules.
     */
    for (uint16_t i = 0; i < tb->nkey; i++) {
        const struct p4_match_spec *m = &up->match[i];
        uint16_t width = key_width(prog, tb, i);
        struct p4_value sv, sm;

        if (m->len > (uint16_t)((width + 7u) / 8u) ||
            sig_bits(m->value, m->len) > width ||
            (tb->key[i].match_type == P4_MATCH_TERNARY &&
             sig_bits(m->mask, m->len) > width))
            return fail(err, err_len, -EINVAL,
                        "key field %u of '%s' is wider than the %u-bit field",
                        i, tb->name, width);
        if (tb->key[i].match_type == P4_MATCH_LPM && m->prefix_bits > width)
            return fail(err, err_len, -EINVAL,
                        "key field %u of '%s' has a %u-bit prefix, the field "
                        "is %u bits",
                        i, tb->name, m->prefix_bits, width);
        if (tb->key[i].match_type != P4_MATCH_LPM && m->prefix_bits != 0)
            return fail(err, err_len, -EINVAL,
                        "key field %u of '%s' has a %u-bit prefix, but is "
                        "not an LPM field",
                        i, tb->name, m->prefix_bits);

        spec_value(m, &sv);
        if (tb->key[i].match_type == P4_MATCH_LPM) {
            prefix_mask(width, m->prefix_bits, &sm);
            if (value_outside(&sv, &sm))
                return fail(err, err_len, -EINVAL,
                            "key field %u of '%s' has a bit set past its "
                            "%u-bit prefix",
                            i, tb->name, m->prefix_bits);
        } else if (tb->key[i].match_type == P4_MATCH_TERNARY) {
            spec_mask(m, &sm);
            if (value_outside(&sv, &sm))
                return fail(err, err_len, -EINVAL,
                            "key field %u of '%s' has a bit set outside its "
                            "mask",
                            i, tb->name);
        }
    }

    if (tb->match_type == P4_MATCH_TERNARY && up->priority == 0)
        return fail(err, err_len, -EINVAL,
                    "table '%s' has a ternary key, so an entry needs a "
                    "nonzero priority",
                    tb->name);
    if (tb->match_type != P4_MATCH_TERNARY && up->priority != 0)
        return fail(err, err_len, -EINVAL,
                    "table '%s' has no ternary key, so an entry takes no "
                    "priority, not %u",
                    tb->name, up->priority);

    if (up->type == P4_UPDATE_DELETE) {
        *action = P4_IDX_NONE;
        return 0;
    }

    *action = P4_IDX_NONE;
    for (uint16_t i = 0; i < tb->nactions; i++) {
        if (strcmp(prog->actions[tb->actions[i]].name, up->action) == 0) {
            *action = tb->actions[i];
            break;
        }
    }
    /* An action the table cannot run is a different error from one that
     * does not exist, and a control plane benefits from the distinction. */
    if (*action == P4_IDX_NONE)
        return fail(err, err_len, -EINVAL,
                    "'%s' is not one of table '%s' actions", up->action,
                    tb->name);

    act = &prog->actions[*action];
    if (up->nparams != act->nruntime)
        return fail(err, err_len, -EINVAL,
                    "action '%s' takes %u arguments, the entry gives %u",
                    act->name, act->nruntime, up->nparams);

    /* An argument wider than the action's parameter would be truncated on
     * use, which is a silently different entry from the one asked for. */
    for (uint16_t i = 0; i < up->nparams; i++) {
        uint16_t max_bytes = (uint16_t)((act->runtime_bits[i] + 7u) / 8u);

        if (up->params[i].len > max_bytes ||
            sig_bits(up->params[i].value, up->params[i].len) >
                act->runtime_bits[i])
            return fail(err, err_len, -EINVAL,
                        "argument %u of '%s' is wider than its %u-bit "
                        "parameter",
                        i, act->name, act->runtime_bits[i]);
    }

    return 0;
}

int p4_entries_update(struct p4_entries *ent, const struct p4_program *prog,
                      const struct p4_update *up, char *err, size_t err_len)
{
    /* Initialised so a future path through resolve() that forgets one
     * cannot leave an index undefined here. */
    p4_idx_t pipe = 0, table = 0, action = P4_IDX_NONE;
    struct p4_table_entries *tbl_e;
    const struct p4_table *tb;
    struct p4_entry *found = NULL;
    struct p4_update masked;
    struct p4_entry_key keys[P4_MAX_TABLE_KEY];
    int rc;

    if (err && err_len)
        err[0] = '\0';
    if (!ent || !prog || !up)
        return -EINVAL;
    if (up->type > P4_UPDATE_DELETE)
        return fail(err, err_len, -EINVAL, "unknown update type %u", up->type);
    if (up->nmatch > P4_MAX_TABLE_KEY || up->nparams > P4_MAX_RUNTIME_ARGS)
        return fail(err, err_len, -EINVAL, "the entry is too large");

    rc = resolve(prog, up, &pipe, &table, &action, err, err_len);
    if (rc)
        return rc;

    tb = &prog->pipelines[pipe].tables[table];
    tbl_e = &ent->t[pipe][table];

    /*
     * bmv2 puts an entry's key under the program's key mask before storing
     * or comparing it, as it does a packet's: under 0x00ff, 0x1234 is
     * 0x0034, and the same row. P4Runtime knows nothing of the mask, so
     * this is what a value it accepts means. resolve() has checked the
     * value as given, and the rest of this works on the masked one.
     */
    masked = *up;
    for (uint16_t i = 0; i < tb->nkey; i++) {
        if (!tb->key[i].has_mask)
            continue;
        for (size_t b = 0; b < P4_MATCH_BYTES; b++)
            masked.match[i].value[b] &= tb->key[i].mask[b];
    }
    up = &masked;
    entry_keys(prog, tb, up, keys);

    for (uint32_t i = 0; i < tbl_e->n; i++) {
        if (same_key(tb, &tbl_e->e[i], keys, up->priority)) {
            found = &tbl_e->e[i];
            break;
        }
    }

    if (up->type == P4_UPDATE_DELETE) {
        struct p4_entry *last;

        if (!found)
            return fail(err, err_len, -ENOENT, "no such entry in '%s'",
                        tb->name);
        /* The last entry fills the hole, keeping them packed. */
        last = &tbl_e->e[tbl_e->n - 1u];
        if (found != last)
            *found = *last;
        memset(last, 0, sizeof(*last));
        tbl_e->n--;
        return 0;
    }

    if (up->type == P4_UPDATE_MODIFY) {
        if (!found)
            return fail(err, err_len, -ENOENT, "no such entry in '%s'",
                        tb->name);
    } else {
        if (found)
            return fail(err, err_len, -EEXIST, "'%s' already has that entry",
                        tb->name);
        /*
         * The table's memory is taken on the first insert, not at load:
         * a program with large tables and nothing in them should cost
         * nothing.
         */
        if (tbl_e->cap == 0) {
            uint32_t cap = tb->max_size ? tb->max_size : 1u;

            tbl_e->e = calloc(cap, sizeof(*tbl_e->e));
            if (!tbl_e->e)
                return fail(err, err_len, -ENOMEM,
                            "out of memory for table '%s'", tb->name);
            tbl_e->cap = cap;
        }
        if (tbl_e->n == tbl_e->cap)
            return fail(err, err_len, -ENOSPC, "table '%s' is full (%u)",
                        tb->name, tbl_e->cap);
        found = &tbl_e->e[tbl_e->n++];
    }

    memset(found, 0, sizeof(*found));
    found->action = action;
    found->priority = up->priority;
    /* The loader refuses an LPM table without exactly one LPM key, and
     * resolve() an LPM entry with a priority. */
    found->rank = (tb->match_type == P4_MATCH_LPM)
                      ? up->match[tb->lpm_key].prefix_bits
                      : up->priority;
    found->nparams = up->nparams;
    for (uint16_t i = 0; i < tb->nkey; i++)
        found->key[i] = keys[i];
    for (uint16_t i = 0; i < up->nparams; i++)
        found->params[i] = up->params[i];

    return 0;
}

const struct p4_entry *p4_entries_lookup(const struct p4_entries *ent,
                                         const struct p4_program *prog,
                                         p4_idx_t pipe, p4_idx_t table,
                                         const struct p4_value *keys)
{
    const struct p4_table_entries *tbl_e;
    const struct p4_table *tb;
    const struct p4_entry *best = NULL;
    uint8_t key[P4_MAX_TABLE_KEY * P4_MATCH_BYTES];

    if (!ent || !prog || pipe >= prog->npipelines)
        return NULL;
    tb = &prog->pipelines[pipe].tables[table];
    tbl_e = &ent->t[pipe][table];

    /*
     * Once per lookup, not per entry: the packet's key under the program's
     * key mask, as bmv2 builds it; every entry's key is already under it
     * (p4_entries_update()).  The mask is right-aligned like the value.
     */
    for (uint16_t k = 0; k < tb->nkey; k++) {
        uint8_t *field = &key[(size_t)k * P4_MATCH_BYTES];

        low_bytes(&keys[k], field);
        if (tb->key[k].has_mask) {
            for (size_t b = 0; b < P4_MATCH_BYTES; b++)
                field[b] &= tb->key[k].mask[b];
        }
    }

    for (uint32_t i = 0; i < tbl_e->n; i++) {
        const struct p4_entry *e = &tbl_e->e[i];

        if (!entry_matches(e, tb->nkey, key))
            continue;

        /*
         * Exact keys cannot overlap, so the first match is the answer.
         * LPM is decided by prefix length and ternary by priority, and
         * both are "largest wins", so one rank covers them.  Of two
         * ternary entries of one rank, the first found wins, which is
         * wherever deletes have left them.
         */
        if (tb->match_type == P4_MATCH_EXACT)
            return e;
        if (!best || e->rank > best->rank)
            best = e;
    }
    return best;
}
