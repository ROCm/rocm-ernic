/*
 * p4_table.c — the entry store and the match units
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p4_table.h"
#include "p4_value.h"

struct p4_table_entries {
    uint32_t n;   /* entries in use */
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

/* Locate a table by name, over every pipeline. */
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

static void value_and(const struct p4_value *a, const struct p4_value *b,
                      struct p4_value *out)
{
    for (size_t i = 0; i < P4_VALUE_BYTES; i++)
        out->b[i] = (uint8_t)(a->b[i] & b->b[i]);
}

/* Does @entry's key field @i match the packet's @key? */
static bool field_matches(const struct p4_table *tb,
                          const struct p4_program *prog,
                          const struct p4_entry *entry, uint16_t i,
                          const struct p4_value *key)
{
    const struct p4_match_spec *s = &entry->match[i];
    const struct p4_header_type *ht;
    struct p4_value sv, m, a, b;
    uint16_t width;

    spec_value(s, &sv);

    switch (tb->key[i].match_type) {
    case P4_MATCH_EXACT:
        return value_cmp(key, &sv) == 0;

    case P4_MATCH_LPM:
        ht = &prog->header_types[prog->headers[tb->key[i].field.header]
                                     .header_type];
        width = ht->fields[tb->key[i].field.field].bits;
        prefix_mask(width, s->prefix_bits, &m);
        value_and(key, &m, &a);
        value_and(&sv, &m, &b);
        return value_cmp(&a, &b) == 0;

    case P4_MATCH_TERNARY:
        spec_mask(s, &m);
        value_and(key, &m, &a);
        value_and(&sv, &m, &b);
        return value_cmp(&a, &b) == 0;

    default:
        return false;
    }
}

/* Two entries address the same row when every key field is identical. */
static bool same_key(const struct p4_table *tb, const struct p4_entry *e,
                     const struct p4_update *up)
{
    for (uint16_t i = 0; i < tb->nkey; i++) {
        const struct p4_match_spec *a = &e->match[i];
        const struct p4_match_spec *b = &up->match[i];

        if (memcmp(a->value, b->value, P4_MATCH_BYTES) != 0)
            return false;
        if (tb->key[i].match_type == P4_MATCH_LPM &&
            a->prefix_bits != b->prefix_bits)
            return false;
        if (tb->key[i].match_type == P4_MATCH_TERNARY &&
            memcmp(a->mask, b->mask, P4_MATCH_BYTES) != 0)
            return false;
    }
    /* Two ternary rows with the same key but different priorities are
     * different rows, since both can be present and the priority picks. */
    if (tb->match_type == P4_MATCH_TERNARY && e->priority != up->priority)
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

        if (up->params[i].len > max_bytes)
            return fail(err, err_len, -EINVAL,
                        "argument %u of '%s' is %u bytes, the parameter is "
                        "%u bits",
                        i, act->name, up->params[i].len, act->runtime_bits[i]);
    }

    /* Likewise a key field wider than the field it matches, and an LPM
     * prefix longer than the field has bits. */
    for (uint16_t i = 0; i < tb->nkey; i++) {
        const struct p4_header_type *ht =
            &prog->header_types[prog->headers[tb->key[i].field.header]
                                    .header_type];
        uint16_t width = ht->fields[tb->key[i].field.field].bits;

        if (up->match[i].len > (uint16_t)((width + 7u) / 8u))
            return fail(err, err_len, -EINVAL,
                        "key field %u of '%s' is %u bytes, the field is %u "
                        "bits",
                        i, tb->name, up->match[i].len, width);
        if (tb->key[i].match_type == P4_MATCH_LPM &&
            up->match[i].prefix_bits > width)
            return fail(err, err_len, -EINVAL,
                        "key field %u of '%s' has a %u-bit prefix, the field "
                        "is %u bits",
                        i, tb->name, up->match[i].prefix_bits, width);
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

    for (uint32_t i = 0; i < tbl_e->cap; i++) {
        if (tbl_e->e[i].used && same_key(tb, &tbl_e->e[i], up)) {
            found = &tbl_e->e[i];
            break;
        }
    }

    if (up->type == P4_UPDATE_DELETE) {
        if (!found)
            return fail(err, err_len, -ENOENT, "no such entry in '%s'",
                        tb->name);
        memset(found, 0, sizeof(*found));
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
        for (uint32_t i = 0; i < tbl_e->cap && !found; i++) {
            if (!tbl_e->e[i].used)
                found = &tbl_e->e[i];
        }
        if (!found)
            return fail(err, err_len, -ENOSPC, "table '%s' is full (%u)",
                        tb->name, tbl_e->cap);
        tbl_e->n++;
    }

    memset(found, 0, sizeof(*found));
    found->used = true;
    found->action = action;
    found->priority = up->priority;
    found->nparams = up->nparams;
    for (uint16_t i = 0; i < tb->nkey; i++)
        found->match[i] = up->match[i];
    for (uint16_t i = 0; i < up->nparams; i++)
        found->params[i] = up->params[i];

    return 0;
}

uint32_t p4_entries_count(const struct p4_entries *ent, p4_idx_t pipe,
                          p4_idx_t table)
{
    if (!ent || pipe >= P4_MAX_PIPELINES || table >= P4_MAX_TABLES)
        return 0;
    return ent->t[pipe][table].n;
}

const struct p4_entry *p4_entries_lookup(const struct p4_entries *ent,
                                         const struct p4_program *prog,
                                         p4_idx_t pipe, p4_idx_t table,
                                         const struct p4_value *keys)
{
    const struct p4_table_entries *tbl_e;
    const struct p4_table *tb;
    const struct p4_entry *best = NULL;
    uint32_t best_rank = 0;

    if (!ent || !prog || pipe >= prog->npipelines)
        return NULL;
    tb = &prog->pipelines[pipe].tables[table];
    tbl_e = &ent->t[pipe][table];

    for (uint32_t i = 0; i < tbl_e->cap; i++) {
        const struct p4_entry *e = &tbl_e->e[i];
        bool all = true;
        uint32_t rank;

        if (!e->used)
            continue;
        for (uint16_t k = 0; k < tb->nkey && all; k++) {
            if (!field_matches(tb, prog, e, k, &keys[k]))
                all = false;
        }
        if (!all)
            continue;

        /*
         * Exact keys cannot overlap, so the first match is the answer.
         * LPM is decided by prefix length and ternary by priority, and
         * both are "largest wins", so one rank covers them.
         */
        if (tb->match_type == P4_MATCH_EXACT)
            return e;
        rank = (tb->match_type == P4_MATCH_LPM) ? e->match[0].prefix_bits
                                                : e->priority;
        if (!best || rank > best_rank) {
            best = e;
            best_rank = rank;
        }
    }
    return best;
}
