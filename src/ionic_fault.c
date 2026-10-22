/*
 * Fault injection for the ionic emulator: the parser, the deterministic
 * patterns and the per-site fault set.  See ionic_fault.h for the model.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ionic_fault.h"
#include "ionic_status.h"

static const char *const site_names[IONIC_FAULT_SITE_COUNT] = {
    [IONIC_FAULT_SITE_STATS_VALS] = "stats_vals",
};

/*
 * Names the design gives that this build does not implement yet.  They are
 * recognised so a spec written for a later build says "not supported in
 * this build" rather than "unknown".
 */
static const char *const later_sites[] = {
    "eth_rx", "eth_tx", "rdma_rx", "rdma_tx", "qp_stats_vals", "stats_publish",
};
static const char *const later_patterns[] = {"random", "state", "gemodel"};
static const char *const later_gates[] = {"window", "start", "stop", "limit",
                                          "seed"};
/* The other stats-read effects: valid for stats_vals, not built yet. */
static const char *const later_stats_effects[] = {
    "stale", "zero", "reset", "wrap", "delay", "timeout", "hang",
};
/* Effects of other sites, never valid for a stats site. */
static const char *const other_site_effects[] = {
    "drop", "nobuf",   "corrupt", "duplicate",  "pause", "rnr",
    "ecn",  "seq_err", "cnp",     "retransmit", "skip",
};

struct status_name {
    const char *name;
    enum ionic_status_code code;
};

/* Every enum ionic_status_code except SUCCESS, which is not a fault. */
static const struct status_name status_names[] = {
    {"eversion", IONIC_RC_EVERSION}, {"eopcode", IONIC_RC_EOPCODE},
    {"eio", IONIC_RC_EIO},           {"eperm", IONIC_RC_EPERM},
    {"eqid", IONIC_RC_EQID},         {"eqtype", IONIC_RC_EQTYPE},
    {"enoent", IONIC_RC_ENOENT},     {"eintr", IONIC_RC_EINTR},
    {"eagain", IONIC_RC_EAGAIN},     {"enomem", IONIC_RC_ENOMEM},
    {"efault", IONIC_RC_EFAULT},     {"ebusy", IONIC_RC_EBUSY},
    {"eexist", IONIC_RC_EEXIST},     {"einval", IONIC_RC_EINVAL},
    {"enospc", IONIC_RC_ENOSPC},     {"erange", IONIC_RC_ERANGE},
    {"bad_addr", IONIC_RC_BAD_ADDR}, {"dev_cmd", IONIC_RC_DEV_CMD},
    {"enosupp", IONIC_RC_ENOSUPP},   {"error", IONIC_RC_ERROR},
    {"erdma", IONIC_RC_ERDMA},       {"evfid", IONIC_RC_EVFID},
    {"ebad_fw", IONIC_RC_EBAD_FW},
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* The longest piece of a token quoted back in an error message. */
#define ERR_TOKEN_MAX 64

/* A whitespace-delimited token of the spec; not NUL-terminated. */
struct tok {
    const char *p;
    size_t len;
};

__attribute__((format(printf, 3, 4))) static void set_err(char *err,
                                                          size_t err_len,
                                                          const char *fmt, ...)
{
    va_list ap;

    if (!err || err_len == 0)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* The length to quote @t with in a "%.*s" error. */
static int tok_prn(const struct tok *t)
{
    return (int)(t->len > ERR_TOKEN_MAX ? ERR_TOKEN_MAX : t->len);
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

/* Read the next token from *@p into @t; false at the end of the spec. */
static bool next_tok(const char **p, struct tok *t)
{
    const char *s = *p;

    while (*s && is_space(*s))
        s++;
    if (!*s) {
        *p = s;
        return false;
    }
    t->p = s;
    while (*s && !is_space(*s))
        s++;
    t->len = (size_t)(s - t->p);
    *p = s;
    return true;
}

static bool tok_is(const struct tok *t, const char *word)
{
    size_t n = strlen(word);

    return t->len == n && memcmp(t->p, word, n) == 0;
}

static bool tok_in(const struct tok *t, const char *const *words, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (tok_is(t, words[i]))
            return true;
    return false;
}

/*
 * Parse @len bytes at @s as a decimal uint64: digits only, no sign, no
 * wrap.  Returns 0, -1 if not a number, or -2 if out of range.
 */
static int parse_u64(const char *s, size_t len, uint64_t *out)
{
    uint64_t v = 0;

    if (len == 0)
        return -1;
    for (size_t i = 0; i < len; i++) {
        uint64_t d;

        if (s[i] < '0' || s[i] > '9')
            return -1;
        d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10)
            return -2;
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

/* Read the number that must follow keyword @what. */
static int need_u64(const char **p, const char *what, uint64_t *out, char *err,
                    size_t err_len)
{
    struct tok t;
    int rc;

    if (!next_tok(p, &t)) {
        set_err(err, err_len, "%s needs a number", what);
        return -1;
    }
    rc = parse_u64(t.p, t.len, out);
    if (rc == -2) {
        set_err(err, err_len, "number '%.*s' out of range for %s", tok_prn(&t),
                t.p, what);
        return -1;
    }
    if (rc != 0) {
        set_err(err, err_len, "bad number '%.*s' for %s", tok_prn(&t), t.p,
                what);
        return -1;
    }
    return 0;
}

/* Like need_u64(), and the number must be at least 1. */
static int need_count(const char **p, const char *what, uint64_t *out,
                      char *err, size_t err_len)
{
    if (need_u64(p, what, out, err, err_len) != 0)
        return -1;
    if (*out == 0) {
        set_err(err, err_len, "%s must be >= 1", what);
        return -1;
    }
    return 0;
}

/*
 * Parse a sequence, "1x3,0x10,1", optionally in double quotes, into a
 * one-byte-per-event buffer.  Two passes: validate and size, then fill.
 */
static int parse_sequence(const struct tok *t, struct ionic_fault *f, char *err,
                          size_t err_len)
{
    const char *s = t->p;
    size_t len = t->len;

    if (len > 0 && s[0] == '"') {
        if (len < 2 || s[len - 1] != '"') {
            set_err(err, err_len, "sequence has an unclosed quote");
            return -1;
        }
        s++;
        len -= 2;
    }
    if (len > 0 && s[0] == '@') {
        set_err(err, err_len, "sequence @file not supported in this build");
        return -1;
    }
    if (len == 0) {
        set_err(err, err_len, "sequence is empty");
        return -1;
    }

    for (int pass = 0; pass < 2; pass++) {
        uint64_t total = 0;
        size_t i = 0;

        for (;;) {
            size_t j = i;
            struct tok item;
            uint64_t rep = 1;

            while (j < len && s[j] != ',')
                j++;
            item.p = s + i;
            item.len = j - i;
            if (item.len == 0) {
                set_err(err, err_len, "empty item in sequence");
                return -1;
            }
            if ((item.p[0] != '0' && item.p[0] != '1') ||
                (item.len > 1 &&
                 (item.p[1] != 'x' ||
                  parse_u64(item.p + 2, item.len - 2, &rep) != 0 ||
                  rep == 0))) {
                set_err(err, err_len, "bad sequence item '%.*s'",
                        tok_prn(&item), item.p);
                return -1;
            }
            if (rep > IONIC_FAULT_SEQ_MAX - total) {
                set_err(err, err_len, "sequence longer than %u events",
                        IONIC_FAULT_SEQ_MAX);
                return -1;
            }
            if (pass == 1)
                memset(f->seq + total, item.p[0] == '1', (size_t)rep);
            total += rep;
            if (j == len)
                break;
            i = j + 1;
        }

        if (pass == 0) {
            f->seq = malloc((size_t)total);
            if (!f->seq) {
                set_err(err, err_len, "out of memory for sequence");
                return -1;
            }
            f->seq_len = (uint32_t)total;
        }
    }
    return 0;
}

static int parse_pattern(const char **p, struct ionic_fault *f, char *err,
                         size_t err_len)
{
    struct tok t;

    if (!next_tok(p, &t) || tok_is(&t, "effect")) {
        set_err(err, err_len, "pattern required");
        return -1;
    }

    if (tok_is(&t, "every")) {
        f->kind = IONIC_FAULT_PAT_EVERY;
        return need_count(p, "every", &f->n, err, err_len);
    }

    if (tok_is(&t, "after")) {
        const char *save;

        f->kind = IONIC_FAULT_PAT_AFTER;
        f->m = 1;
        if (need_count(p, "after", &f->n, err, err_len) != 0)
            return -1;
        save = *p;
        if (next_tok(p, &t) && tok_is(&t, "count"))
            return need_count(p, "count", &f->m, err, err_len);
        *p = save;
        return 0;
    }

    if (tok_is(&t, "burst")) {
        f->kind = IONIC_FAULT_PAT_BURST;
        if (need_count(p, "burst", &f->n, err, err_len) != 0)
            return -1;
        if (!next_tok(p, &t) || !tok_is(&t, "every")) {
            set_err(err, err_len, "burst N needs 'every M'");
            return -1;
        }
        if (need_count(p, "burst every", &f->m, err, err_len) != 0)
            return -1;
        if (f->n > f->m) {
            set_err(err, err_len, "burst %" PRIu64 " must be <= every %" PRIu64,
                    f->n, f->m);
            return -1;
        }
        return 0;
    }

    if (tok_is(&t, "sequence")) {
        f->kind = IONIC_FAULT_PAT_SEQUENCE;
        if (!next_tok(p, &t)) {
            set_err(err, err_len, "sequence needs a pattern");
            return -1;
        }
        return parse_sequence(&t, f, err, err_len);
    }

    if (tok_in(&t, later_patterns, ARRAY_LEN(later_patterns)))
        set_err(err, err_len, "pattern %.*s not supported in this build",
                tok_prn(&t), t.p);
    else
        set_err(err, err_len, "unknown pattern '%.*s'", tok_prn(&t), t.p);
    return -1;
}

/* Report @t, which was not expected where it stands. */
static void bad_tok(const struct tok *t, char *err, size_t err_len)
{
    if (tok_in(t, later_gates, ARRAY_LEN(later_gates)))
        set_err(err, err_len, "gate %.*s not supported in this build",
                tok_prn(t), t->p);
    else
        set_err(err, err_len, "unexpected token '%.*s'", tok_prn(t), t->p);
}

static int parse_effect(const char **p, struct ionic_fault *f, char *err,
                        size_t err_len)
{
    const char *site = site_names[f->site];
    struct tok t;

    if (!next_tok(p, &t)) {
        set_err(err, err_len, "effect required");
        return -1;
    }
    if (!tok_is(&t, "effect")) {
        bad_tok(&t, err, err_len);
        return -1;
    }
    if (!next_tok(p, &t)) {
        set_err(err, err_len, "effect needs a name");
        return -1;
    }

    if (tok_is(&t, "fail")) {
        f->effect = IONIC_FAULT_EFFECT_FAIL;
        f->status = IONIC_RC_EIO;
        while (next_tok(p, &t)) {
            size_t i;

            if (!tok_is(&t, "status")) {
                bad_tok(&t, err, err_len);
                return -1;
            }
            if (!next_tok(p, &t)) {
                set_err(err, err_len, "status needs a name");
                return -1;
            }
            for (i = 0; i < ARRAY_LEN(status_names); i++)
                if (tok_is(&t, status_names[i].name))
                    break;
            if (i == ARRAY_LEN(status_names)) {
                set_err(err, err_len, "unknown status '%.*s'", tok_prn(&t),
                        t.p);
                return -1;
            }
            f->status = (uint8_t)status_names[i].code;
        }
        return 0;
    }

    if (tok_in(&t, later_stats_effects, ARRAY_LEN(later_stats_effects)))
        set_err(err, err_len, "effect %.*s not supported in this build",
                tok_prn(&t), t.p);
    else if (tok_in(&t, other_site_effects, ARRAY_LEN(other_site_effects)))
        set_err(err, err_len, "effect %.*s not valid for site %s", tok_prn(&t),
                t.p, site);
    else
        set_err(err, err_len, "unknown effect '%.*s'", tok_prn(&t), t.p);
    return -1;
}

int ionic_fault_parse(const char *spec, struct ionic_fault *out, char *err,
                      size_t err_len)
{
    struct ionic_fault f;
    const char *p = spec;
    struct tok t;
    size_t s;

    if (!out) {
        set_err(err, err_len, "no fault to parse into");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    memset(&f, 0, sizeof(f));
    if (!spec || !next_tok(&p, &t)) {
        set_err(err, err_len, "empty fault");
        return -1;
    }

    if (t.len < 2 || t.p[t.len - 1] != ':') {
        set_err(err, err_len, "expected '<site>:' at '%.*s'", tok_prn(&t), t.p);
        return -1;
    }
    t.len--;
    for (s = 0; s < IONIC_FAULT_SITE_COUNT; s++)
        if (tok_is(&t, site_names[s]))
            break;
    if (s == IONIC_FAULT_SITE_COUNT) {
        if (tok_in(&t, later_sites, ARRAY_LEN(later_sites)))
            set_err(err, err_len, "site %.*s not supported in this build",
                    tok_prn(&t), t.p);
        else
            set_err(err, err_len, "unknown site '%.*s'", tok_prn(&t), t.p);
        return -1;
    }
    f.site = (enum ionic_fault_site)s;

    if (parse_pattern(&p, &f, err, err_len) != 0 ||
        parse_effect(&p, &f, err, err_len) != 0) {
        free(f.seq);
        return -1;
    }

    *out = f;
    return 0;
}

void ionic_fault_clear(struct ionic_fault *f)
{
    if (!f)
        return;
    free(f->seq);
    memset(f, 0, sizeof(*f));
}

/* Append to @buf at *@pos, counting what would not fit, as snprintf(). */
__attribute__((format(printf, 4, 5))) static void out_add(char *buf, size_t len,
                                                          size_t *pos,
                                                          const char *fmt, ...)
{
    char *dst = NULL;
    size_t room = 0;
    va_list ap;
    int n;

    if (buf && *pos < len) {
        dst = buf + *pos;
        room = len - *pos;
    }
    va_start(ap, fmt);
    n = vsnprintf(dst, room, fmt, ap);
    va_end(ap);
    if (n > 0)
        *pos += (size_t)n;
}

static const char *status_name(uint8_t status)
{
    for (size_t i = 0; i < ARRAY_LEN(status_names); i++)
        if ((unsigned int)status_names[i].code == status)
            return status_names[i].name;
    return NULL;
}

int ionic_fault_format(const struct ionic_fault *f, char *buf, size_t len)
{
    const char *st;
    size_t pos = 0;

    if (buf && len > 0)
        buf[0] = '\0';
    if (!f)
        return 0;

    out_add(buf, len, &pos, "%s: ", ionic_fault_site_name(f->site));
    switch (f->kind) {
    case IONIC_FAULT_PAT_EVERY:
        out_add(buf, len, &pos, "every %" PRIu64, f->n);
        break;
    case IONIC_FAULT_PAT_AFTER:
        out_add(buf, len, &pos, "after %" PRIu64 " count %" PRIu64, f->n, f->m);
        break;
    case IONIC_FAULT_PAT_BURST:
        out_add(buf, len, &pos, "burst %" PRIu64 " every %" PRIu64, f->n, f->m);
        break;
    case IONIC_FAULT_PAT_SEQUENCE:
        out_add(buf, len, &pos, "sequence \"");
        for (uint32_t i = 0; i < f->seq_len;) {
            bool fire = f->seq[i] != 0;
            uint32_t j = i;

            while (j < f->seq_len && (f->seq[j] != 0) == fire)
                j++;
            out_add(buf, len, &pos, "%s%c", i ? "," : "", fire ? '1' : '0');
            if (j - i > 1)
                out_add(buf, len, &pos, "x%" PRIu32, j - i);
            i = j;
        }
        out_add(buf, len, &pos, "\"");
        break;
    default:
        out_add(buf, len, &pos, "unknown");
        break;
    }

    if (f->effect == IONIC_FAULT_EFFECT_FAIL) {
        st = status_name(f->status);
        if (st)
            out_add(buf, len, &pos, " effect fail status %s", st);
        else
            out_add(buf, len, &pos, " effect fail status %u",
                    (unsigned int)f->status);
    } else {
        out_add(buf, len, &pos, " effect unknown");
    }

    return pos > INT_MAX ? -1 : (int)pos;
}

bool ionic_fault_event(struct ionic_fault *f)
{
    uint64_t k;
    bool fire;

    if (!f)
        return false;
    /* Saturated: the event index can no longer be told apart, so a
     * counting pattern would misfire.  Stop rather than wrap. */
    if (f->eligible == UINT64_MAX)
        return false;
    k = ++f->eligible;

    switch (f->kind) {
    case IONIC_FAULT_PAT_EVERY:
        fire = f->n != 0 && k % f->n == 0;
        break;
    case IONIC_FAULT_PAT_AFTER:
        fire = k >= f->n && k - f->n < f->m;
        break;
    case IONIC_FAULT_PAT_BURST:
        fire = f->m != 0 && (k - 1) % f->m < f->n;
        break;
    case IONIC_FAULT_PAT_SEQUENCE:
        fire = f->seq_len != 0 && f->seq[(k - 1) % f->seq_len] != 0;
        break;
    default:
        fire = false;
        break;
    }

    if (fire && f->fired != UINT64_MAX)
        f->fired++;
    return fire;
}

struct ionic_fault_set *ionic_fault_set_new(void)
{
    return calloc(1, sizeof(struct ionic_fault_set));
}

void ionic_fault_set_free(struct ionic_fault_set *set)
{
    if (!set)
        return;
    for (size_t i = 0; i < IONIC_FAULT_SITE_COUNT; i++) {
        ionic_fault_clear(set->site[i]);
        free(set->site[i]);
    }
    free(set);
}

int ionic_fault_set_add(struct ionic_fault_set *set, const char *spec,
                        char *err, size_t err_len)
{
    struct ionic_fault f;
    struct ionic_fault *slot;

    if (!set) {
        set_err(err, err_len, "no fault set");
        return -1;
    }
    if (ionic_fault_parse(spec, &f, err, err_len) != 0)
        return -1;
    if (set->site[f.site]) {
        set_err(err, err_len, "duplicate fault for site %s",
                site_names[f.site]);
        ionic_fault_clear(&f);
        return -1;
    }
    slot = malloc(sizeof(*slot));
    if (!slot) {
        set_err(err, err_len, "out of memory for fault");
        ionic_fault_clear(&f);
        return -1;
    }
    *slot = f;
    set->site[f.site] = slot;
    return 0;
}

struct ionic_fault *ionic_fault_set_get(const struct ionic_fault_set *set,
                                        enum ionic_fault_site site)
{
    if (!set || (unsigned int)site >= IONIC_FAULT_SITE_COUNT)
        return NULL;
    return set->site[site];
}

const char *ionic_fault_site_name(enum ionic_fault_site site)
{
    if ((unsigned int)site >= IONIC_FAULT_SITE_COUNT)
        return "unknown";
    return site_names[site];
}
