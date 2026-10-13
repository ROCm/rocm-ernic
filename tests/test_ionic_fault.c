/*
 * Unit tests for the fault-injection core (src/ionic_fault.c): the
 * --inject parser, the deterministic patterns and the per-site fault set.
 *
 * One table.  Each row names a check: run a parsed fault over a stretch of
 * events and compare where it fired, compare the parsed fields, expect a
 * parse error naming the bad token, or round-trip the canonical form.  A
 * few checks that need their own set-up (the empty set, a duplicate site,
 * a truncated format buffer) are rows too, so every case prints the same
 * "ok/FAIL <category>: <description>; expected: <expected>" line.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ionic_fault.h"
#include "ionic_status.h"

static int failures;

enum check {
    CHECK_FIRES,     /* fire pattern over a run of events */
    CHECK_FIELDS,    /* the parsed fields */
    CHECK_ERROR,     /* rejected, with a message containing want_err */
    CHECK_ROUNDTRIP, /* format, then parse back to the same fault */
    CHECK_NO_FAULT,  /* the no-fault paths count nothing and never fire */
    CHECK_DUPLICATE, /* a second fault on the same site */
    CHECK_TRUNCATE,  /* format into a buffer of buf_len bytes */
    CHECK_STATUSES,  /* every status name parses to its code */
    CHECK_SITES,     /* one fault on each site in one set */
};

struct fault_case {
    const char *category;
    const char *description;
    const char *expected;
    enum check check;
    const char *spec;

    /* CHECK_FIRES */
    uint64_t start;           /* eligible events already counted */
    size_t nevents;           /* events to run; 0 = strlen(fires) */
    const char *fires;        /* one '1'/'0' per event, or NULL to use want */
    bool (*want)(uint64_t k); /* fires on event index k (from 1) */
    uint64_t want_fired;

    /* CHECK_FIELDS; site and effect also for CHECK_DUPLICATE */
    enum ionic_fault_site site;
    enum ionic_fault_effect effect;
    enum ionic_fault_pattern_kind kind;
    uint64_t n, m;
    uint32_t seq_len;
    uint8_t status;

    /* CHECK_ERROR, CHECK_DUPLICATE */
    const char *want_err;

    /* CHECK_ROUNDTRIP: the canonical text, or NULL to skip that compare */
    const char *want_text;

    /* CHECK_TRUNCATE */
    size_t buf_len;
};

/* ---- Reference fire predicates ----------------------------------------- */

static bool every_100(uint64_t k)
{
    return k % 100 == 0;
}

static bool first_5_of_100(uint64_t k)
{
    return k % 100 >= 1 && k % 100 <= 5;
}

static bool always(uint64_t k)
{
    (void)k;
    return true;
}

static bool never(uint64_t k)
{
    (void)k;
    return false;
}

static bool only_last(uint64_t k)
{
    return k == UINT64_MAX;
}

/* ---- The table --------------------------------------------------------- */

#define SITE        "stats_vals: "
#define ETH_RX_SITE IONIC_FAULT_SITE_ETH_RX
#define ETH_TX_SITE IONIC_FAULT_SITE_ETH_TX

static const struct fault_case cases[] = {
    /* positive: patterns */
    {"positive", "'every 100' over 1000 events",
     "fires 10 times, at events 100, 200, ... 1000", CHECK_FIRES,
     SITE "every 100 effect fail", .nevents = 1000, .want = every_100,
     .want_fired = 10},
    {"positive", "'after 3'", "fires only on event 3", CHECK_FIRES,
     SITE "after 3 effect fail", .fires = "0010000000", .want_fired = 1},
    {"positive", "'after 3 count 2'", "fires on events 3 and 4 only",
     CHECK_FIRES, SITE "after 3 count 2 effect fail", .fires = "0011000000",
     .want_fired = 2},
    {"positive", "'burst 5 every 100' over 1000 events",
     "50 fires, in 10 runs of 5, each starting at 1 mod 100", CHECK_FIRES,
     SITE "burst 5 every 100 effect fail", .nevents = 1000,
     .want = first_5_of_100, .want_fired = 50},
    {"positive", "'sequence \"1x3,0x2\"' over 10 events",
     "1110011100: the sequence loops", CHECK_FIRES,
     SITE "sequence \"1x3,0x2\" effect fail", .fires = "1110011100",
     .want_fired = 6},
    {"positive", "'sequence 1,0,0x2,1' unquoted",
     "10001 then loops: 1000110001", CHECK_FIRES,
     SITE "sequence 1,0,0x2,1 effect fail", .fires = "1000110001",
     .want_fired = 4},

    /* positive: parsing */
    {"positive", "CLI 'stats_vals: every 1 effect fail'",
     "site stats_vals, every 1, effect fail, status eio (3)", CHECK_FIELDS,
     SITE "every 1 effect fail", .kind = IONIC_FAULT_PAT_EVERY, .n = 1,
     .status = IONIC_RC_EIO},
    {"positive", "'effect fail status enoent'", "status ENOENT (7)",
     CHECK_FIELDS, SITE "every 1 effect fail status enoent",
     .kind = IONIC_FAULT_PAT_EVERY, .n = 1, .status = IONIC_RC_ENOENT},
    {"positive", "'after 5' without count", "after 5 count 1", CHECK_FIELDS,
     SITE "after 5 effect fail", .kind = IONIC_FAULT_PAT_AFTER, .n = 5, .m = 1,
     .status = IONIC_RC_EIO},
    {"positive", "'burst 2 every 7'", "burst n 2, m 7", CHECK_FIELDS,
     SITE "burst 2 every 7 effect fail", .kind = IONIC_FAULT_PAT_BURST, .n = 2,
     .m = 7, .status = IONIC_RC_EIO},
    {"positive", "'sequence \"1x3,0x10,1\"'", "a 14-event sequence",
     CHECK_FIELDS, SITE "sequence \"1x3,0x10,1\" effect fail",
     .kind = IONIC_FAULT_PAT_SEQUENCE, .seq_len = 14, .status = IONIC_RC_EIO},
    {"positive", "canonical form of 'every 100'", "re-parses to the same fault",
     CHECK_ROUNDTRIP, SITE "every 100 effect fail status eversion",
     .want_text = "stats_vals: every 100 effect fail status eversion"},
    {"positive", "canonical form of 'after 3'",
     "spells out count 1 and the default status", CHECK_ROUNDTRIP,
     SITE "after 3 effect fail",
     .want_text = "stats_vals: after 3 count 1 effect fail status eio"},
    {"positive", "canonical form of 'burst 5 every 100'",
     "re-parses to the same fault", CHECK_ROUNDTRIP,
     SITE "burst 5 every 100 effect fail",
     .want_text = "stats_vals: burst 5 every 100 effect fail status eio"},
    {"positive", "canonical form of a sequence",
     "runs merged and quoted: \"1x3,0x12,1\"", CHECK_ROUNDTRIP,
     SITE "sequence 1x2,1,0x10,0x2,1 effect fail",
     .want_text = "stats_vals: sequence \"1x3,0x12,1\" effect fail status eio"},
    {"positive", "'eth_rx: every 10 effect drop'",
     "site eth_rx, every 10, effect drop, status 0", CHECK_FIELDS,
     "eth_rx: every 10 effect drop", .site = ETH_RX_SITE,
     .effect = IONIC_FAULT_EFFECT_DROP, .kind = IONIC_FAULT_PAT_EVERY, .n = 10},
    {"positive", "'eth_rx: burst 3 every 50 effect nobuf'",
     "site eth_rx, burst 3 every 50, effect nobuf", CHECK_FIELDS,
     "eth_rx: burst 3 every 50 effect nobuf", .site = ETH_RX_SITE,
     .effect = IONIC_FAULT_EFFECT_NOBUF, .kind = IONIC_FAULT_PAT_BURST, .n = 3,
     .m = 50},
    {"positive", "'eth_tx: after 100 count 5 effect drop'",
     "site eth_tx, after 100 count 5, effect drop", CHECK_FIELDS,
     "eth_tx: after 100 count 5 effect drop", .site = ETH_TX_SITE,
     .effect = IONIC_FAULT_EFFECT_DROP, .kind = IONIC_FAULT_PAT_AFTER, .n = 100,
     .m = 5},
    {"positive", "'eth_rx: every 3 effect drop' over 9 events",
     "001001001: patterns are the same on every site", CHECK_FIRES,
     "eth_rx: every 3 effect drop", .fires = "001001001", .want_fired = 3},
    {"positive", "canonical form of an eth_rx drop",
     "re-parses to the same fault", CHECK_ROUNDTRIP,
     "eth_rx: every 10 effect drop",
     .want_text = "eth_rx: every 10 effect drop"},
    {"positive", "canonical form of an eth_rx nobuf",
     "spells out count 1; no status", CHECK_ROUNDTRIP,
     "eth_rx: after 2 effect nobuf",
     .want_text = "eth_rx: after 2 count 1 effect nobuf"},
    {"positive", "canonical form of an eth_tx sequence drop",
     "quoted: \"1,0x3\"", CHECK_ROUNDTRIP, "eth_tx: sequence 1,0x3 effect drop",
     .want_text = "eth_tx: sequence \"1,0x3\" effect drop"},

    /* negative */
    {"negative", "unknown site 'foo: every 1 effect fail'",
     "rejected, naming foo", CHECK_ERROR, "foo: every 1 effect fail",
     .want_err = "unknown site 'foo'"},
    {"negative", "site not yet built 'rdma_rx: every 1 effect rnr'",
     "rejected as not supported in this build", CHECK_ERROR,
     "rdma_rx: every 1 effect rnr",
     .want_err = "site rdma_rx not supported in this build"},
    {"negative", "site without its colon", "rejected: '<site>:' expected",
     CHECK_ERROR, "stats_vals every 1 effect fail",
     .want_err = "expected '<site>:' at 'stats_vals'"},
    {"negative", "unknown effect 'explode'", "rejected, naming explode",
     CHECK_ERROR, SITE "every 1 effect explode",
     .want_err = "unknown effect 'explode'"},
    {"negative", "effect invalid for the site ('effect drop')",
     "rejected: not valid for site stats_vals", CHECK_ERROR,
     SITE "every 1 effect drop",
     .want_err = "effect drop not valid for site stats_vals"},
    {"negative", "stats effect not yet built ('effect stale')",
     "rejected as not supported in this build", CHECK_ERROR,
     SITE "every 1 effect stale",
     .want_err = "effect stale not supported in this build"},
    {"negative", "'stats_vals: every 1 effect nobuf'",
     "rejected: not valid for site stats_vals", CHECK_ERROR,
     SITE "every 1 effect nobuf",
     .want_err = "effect nobuf not valid for site stats_vals"},
    {"negative", "'eth_rx: every 1 effect fail'",
     "rejected: not valid for site eth_rx", CHECK_ERROR,
     "eth_rx: every 1 effect fail",
     .want_err = "effect fail not valid for site eth_rx"},
    {"negative", "'eth_tx: every 1 effect nobuf'",
     "rejected: nobuf is an Rx effect", CHECK_ERROR,
     "eth_tx: every 1 effect nobuf",
     .want_err = "effect nobuf not valid for site eth_tx"},
    {"negative", "'eth_rx: every 1 effect rnr'",
     "rejected: rnr belongs to rdma_rx", CHECK_ERROR,
     "eth_rx: every 1 effect rnr",
     .want_err = "effect rnr not valid for site eth_rx"},
    {"negative", "eth_rx effect not yet built ('effect corrupt')",
     "rejected as not supported in this build", CHECK_ERROR,
     "eth_rx: every 1 effect corrupt",
     .want_err = "effect corrupt not supported in this build"},
    {"negative", "'eth_rx: every 1 effect drop status eio'",
     "rejected: drop takes no status", CHECK_ERROR,
     "eth_rx: every 1 effect drop status eio",
     .want_err = "unexpected token 'status'"},
    {"negative", "trailing token after 'eth_tx ... effect drop'",
     "rejected, naming junk", CHECK_ERROR, "eth_tx: every 1 effect drop junk",
     .want_err = "unexpected token 'junk'"},
    {"negative", "'every 0'", "rejected: every must be >= 1", CHECK_ERROR,
     SITE "every 0 effect fail", .want_err = "every must be >= 1"},
    {"negative", "'after 0'", "rejected: after must be >= 1", CHECK_ERROR,
     SITE "after 0 effect fail", .want_err = "after must be >= 1"},
    {"negative", "'after 3 count 0'", "rejected: count must be >= 1",
     CHECK_ERROR, SITE "after 3 count 0 effect fail",
     .want_err = "count must be >= 1"},
    {"negative", "'burst 6 every 5'", "rejected: burst must be <= every",
     CHECK_ERROR, SITE "burst 6 every 5 effect fail",
     .want_err = "burst 6 must be <= every 5"},
    {"negative", "'burst 5' without every", "rejected: needs 'every M'",
     CHECK_ERROR, SITE "burst 5 effect fail",
     .want_err = "burst N needs 'every M'"},
    {"negative", "missing effect", "rejected: effect required", CHECK_ERROR,
     SITE "every 5", .want_err = "effect required"},
    {"negative", "missing pattern", "rejected: pattern required", CHECK_ERROR,
     SITE "effect fail", .want_err = "pattern required"},
    {"negative", "'effect' with no name", "rejected: effect needs a name",
     CHECK_ERROR, SITE "every 5 effect", .want_err = "effect needs a name"},
    {"negative", "trailing token 'junk'", "rejected, naming junk", CHECK_ERROR,
     SITE "every 5 effect fail junk", .want_err = "unexpected token 'junk'"},
    {"negative", "a gate before E03 ('limit 3')",
     "rejected: gate limit not supported in this build", CHECK_ERROR,
     SITE "every 5 effect fail limit 3",
     .want_err = "gate limit not supported in this build"},
    {"negative", "random pattern before E02 ('random 1%')",
     "rejected: pattern random not supported in this build", CHECK_ERROR,
     SITE "random 1% effect fail",
     .want_err = "pattern random not supported in this build"},
    {"negative", "unknown pattern 'often'", "rejected, naming often",
     CHECK_ERROR, SITE "often effect fail",
     .want_err = "unknown pattern 'often'"},
    {"negative", "number overflow 'every 18446744073709551616'",
     "rejected as out of range, no wrap", CHECK_ERROR,
     SITE "every 18446744073709551616 effect fail",
     .want_err = "number '18446744073709551616' out of range for every"},
    {"negative", "signed number 'every -1'", "rejected as a bad number",
     CHECK_ERROR, SITE "every -1 effect fail",
     .want_err = "bad number '-1' for every"},
    {"negative", "number with a suffix 'every 10x'", "rejected as a bad number",
     CHECK_ERROR, SITE "every 10x effect fail",
     .want_err = "bad number '10x' for every"},
    {"negative", "unknown status 'ewhatever'", "rejected, naming it",
     CHECK_ERROR, SITE "every 1 effect fail status ewhatever",
     .want_err = "unknown status 'ewhatever'"},
    {"negative", "status success ('status success')",
     "rejected: success is not a fault", CHECK_ERROR,
     SITE "every 1 effect fail status success",
     .want_err = "unknown status 'success'"},
    {"negative", "'status' with no name", "rejected: status needs a name",
     CHECK_ERROR, SITE "every 1 effect fail status",
     .want_err = "status needs a name"},
    {"negative", "sequence from a file before E09",
     "rejected: @file not supported in this build", CHECK_ERROR,
     SITE "sequence @trace.txt effect fail",
     .want_err = "sequence @file not supported in this build"},
    {"negative", "sequence item '2'", "rejected, naming the item", CHECK_ERROR,
     SITE "sequence 1,2 effect fail", .want_err = "bad sequence item '2'"},
    {"negative", "sequence item '1x0'", "rejected: repeat must be >= 1",
     CHECK_ERROR, SITE "sequence 1x0 effect fail",
     .want_err = "bad sequence item '1x0'"},
    {"negative", "sequence with an empty item", "rejected", CHECK_ERROR,
     SITE "sequence 1,,0 effect fail", .want_err = "empty item in sequence"},
    {"negative", "sequence with a trailing comma", "rejected", CHECK_ERROR,
     SITE "sequence 1,0, effect fail", .want_err = "empty item in sequence"},
    {"negative", "sequence with an unclosed quote", "rejected", CHECK_ERROR,
     SITE "sequence \"1x3 effect fail",
     .want_err = "sequence has an unclosed quote"},
    {"negative", "empty quoted sequence", "rejected: sequence is empty",
     CHECK_ERROR, SITE "sequence \"\" effect fail",
     .want_err = "sequence is empty"},
    {"negative", "sequence over 65536 events",
     "rejected: longer than 65536 events", CHECK_ERROR,
     SITE "sequence 1x65536,0 effect fail",
     .want_err = "sequence longer than 65536 events"},
    {"negative", "empty spec", "rejected: empty fault", CHECK_ERROR, "",
     .want_err = "empty fault"},
    {"negative", "whitespace-only spec", "rejected: empty fault", CHECK_ERROR,
     " \t ", .want_err = "empty fault"},
    {"negative", "NULL spec", "rejected: empty fault", CHECK_ERROR, NULL,
     .want_err = "empty fault"},

    /* boundary */
    {"boundary", "'every 1'", "fires on every event", CHECK_FIRES,
     SITE "every 1 effect fail", .fires = "1111111111", .want_fired = 10},
    {"boundary", "'every 18446744073709551615' over 10 events",
     "never fires; no overflow", CHECK_FIRES,
     SITE "every 18446744073709551615 effect fail", .nevents = 10,
     .want = never, .want_fired = 0},
    {"boundary", "'every UINT64_MAX' at the last countable event",
     "fires on event UINT64_MAX only", CHECK_FIRES,
     SITE "every 18446744073709551615 effect fail", .start = UINT64_MAX - 3,
     .nevents = 6, .want = only_last, .want_fired = 1},
    {"boundary", "'after 1 count 1'", "fires only on event 1", CHECK_FIRES,
     SITE "after 1 count 1 effect fail", .fires = "1000000000",
     .want_fired = 1},
    {"boundary", "'burst 7 every 7'", "always fires", CHECK_FIRES,
     SITE "burst 7 every 7 effect fail", .nevents = 50, .want = always,
     .want_fired = 50},
    {"boundary", "'burst 1 every 1'", "always fires", CHECK_FIRES,
     SITE "burst 1 every 1 effect fail", .fires = "11111", .want_fired = 5},
    {"boundary", "'sequence \"0\"'", "never fires", CHECK_FIRES,
     SITE "sequence \"0\" effect fail", .nevents = 20, .want = never,
     .want_fired = 0},
    {"boundary", "sequence of exactly 65536 events", "accepted", CHECK_FIELDS,
     SITE "sequence 1x65535,0 effect fail", .kind = IONIC_FAULT_PAT_SEQUENCE,
     .seq_len = 65536, .status = IONIC_RC_EIO},
    {"boundary", "'every 18446744073709551615'", "accepted as UINT64_MAX",
     CHECK_FIELDS, SITE "every 18446744073709551615 effect fail",
     .kind = IONIC_FAULT_PAT_EVERY, .n = UINT64_MAX, .status = IONIC_RC_EIO},
    {"boundary", "format into a 10-byte buffer",
     "returns the full length; 9 bytes and a NUL written", CHECK_TRUNCATE,
     SITE "every 1 effect fail", .buf_len = 10},
    {"boundary", "format into a 0-byte buffer",
     "returns the full length; nothing written", CHECK_TRUNCATE,
     SITE "every 1 effect fail", .buf_len = 0},

    {"negative", "bad effect after allocating maximum sequence",
     "error and no owned sequence", CHECK_ERROR,
     SITE "sequence 1x65536 effect explode", .want_err = "unknown effect"},
    {"negative", "bad status after allocating sequence",
     "error and no owned sequence", CHECK_ERROR,
     SITE "sequence 1,0,1 effect fail status nope",
     .want_err = "unknown status"},

    /* corner */
    {"corner", "no faults configured",
     "the site pointer is NULL; the hook never fires", CHECK_NO_FAULT,
     .spec = NULL},
    {"corner", "eligible and fired after 1000 events with 'every 100'",
     "eligible 1000, fired 10", CHECK_FIRES, SITE "every 100 effect fail",
     .nevents = 1000, .want = every_100, .want_fired = 10},
    {"corner", "the same --inject twice for one site",
     "rejected until E03 adds labels", CHECK_DUPLICATE,
     SITE "every 1 effect fail",
     .want_err = "duplicate fault for site stats_vals"},
    {"corner", "the same eth_rx fault twice", "rejected: one fault per site",
     CHECK_DUPLICATE, "eth_rx: every 1 effect nobuf", .site = ETH_RX_SITE,
     .want_err = "duplicate fault for site eth_rx"},
    {"corner", "one fault on each of stats_vals, eth_rx and eth_tx",
     "all three accepted into one set, each at its own site", CHECK_SITES,
     .spec = NULL},
    {"corner", "event counter near UINT64_MAX with 'after 1'",
     "saturates at UINT64_MAX; never wraps to re-fire", CHECK_FIRES,
     SITE "after 1 effect fail", .start = UINT64_MAX - 2, .nevents = 5,
     .want = never, .want_fired = 0},
    {"corner", "tabs and repeated spaces between tokens",
     "parsed as single spaces", CHECK_FIELDS,
     "  stats_vals:\tevery   3\t effect  fail  status\tebusy ",
     .kind = IONIC_FAULT_PAT_EVERY, .n = 3, .status = IONIC_RC_EBUSY},
    {"corner", "leading zeros 'every 007'", "every 7", CHECK_FIELDS,
     SITE "every 007 effect fail", .kind = IONIC_FAULT_PAT_EVERY, .n = 7,
     .status = IONIC_RC_EIO},
    {"corner", "status given twice", "the last one wins", CHECK_FIELDS,
     SITE "every 1 effect fail status eio status eagain",
     .kind = IONIC_FAULT_PAT_EVERY, .n = 1, .status = IONIC_RC_EAGAIN},
    {"corner", "a 65536-event sequence round trip",
     "re-parses to the same 65536 bytes", CHECK_ROUNDTRIP,
     SITE "sequence 1x65535,0 effect fail", .want_text = NULL},
    {"corner", "alternating sequence round trip",
     "no run merged across different values", CHECK_ROUNDTRIP,
     SITE "sequence 1,0,1,0,1x2 effect fail",
     .want_text =
         "stats_vals: sequence \"1,0,1,0,1x2\" effect fail status eio"},
    {"corner", "every status name", "each parses to its ionic_if.h code",
     CHECK_STATUSES, .spec = NULL},
};

/* ---- Checks ------------------------------------------------------------ */

static bool same_fault(const struct ionic_fault *a, const struct ionic_fault *b)
{
    if (a->site != b->site || a->kind != b->kind || a->n != b->n ||
        a->m != b->m || a->effect != b->effect || a->status != b->status ||
        a->seq_len != b->seq_len)
        return false;
    return a->seq_len == 0 || memcmp(a->seq, b->seq, a->seq_len) == 0;
}

static bool check_fires(const struct fault_case *c, char *why, size_t len)
{
    struct ionic_fault f;
    char err[128] = "";
    size_t n = c->nevents ? c->nevents : strlen(c->fires);
    uint64_t want_eligible;

    if (ionic_fault_parse(c->spec, &f, err, sizeof(err)) != 0) {
        snprintf(why, len, "spec rejected: %s", err);
        return false;
    }
    f.eligible = c->start;
    for (size_t i = 0; i < n; i++) {
        uint64_t k = c->start + i + 1;
        bool want =
            c->fires ? c->fires[i] == '1' : (k > c->start && c->want(k));
        bool got = ionic_fault_event(&f);

        if (got != want) {
            snprintf(why, len,
                     "event %zu (index %" PRIu64 "): fired %d, want %d", i + 1,
                     k, got, want);
            ionic_fault_clear(&f);
            return false;
        }
    }
    want_eligible = UINT64_MAX - c->start < n ? UINT64_MAX : c->start + n;
    if (f.fired != c->want_fired || f.eligible != want_eligible) {
        snprintf(why, len,
                 "fired %" PRIu64 " (want %" PRIu64 "), eligible %" PRIu64
                 " (want %" PRIu64 ")",
                 f.fired, c->want_fired, f.eligible, want_eligible);
        ionic_fault_clear(&f);
        return false;
    }
    ionic_fault_clear(&f);
    return true;
}

static bool check_fields(const struct fault_case *c, char *why, size_t len)
{
    struct ionic_fault f;
    char err[128] = "";
    bool ok;

    if (ionic_fault_parse(c->spec, &f, err, sizeof(err)) != 0) {
        snprintf(why, len, "spec rejected: %s", err);
        return false;
    }
    ok = f.site == c->site && f.kind == c->kind && f.n == c->n && f.m == c->m &&
         f.seq_len == c->seq_len && f.effect == c->effect &&
         f.status == c->status && f.eligible == 0 && f.fired == 0 &&
         (c->seq_len == 0) == (f.seq == NULL);
    if (!ok)
        snprintf(why, len,
                 "site %d effect %d kind %d n %" PRIu64 " m %" PRIu64
                 " seq_len %" PRIu32 " status %u",
                 (int)f.site, (int)f.effect, (int)f.kind, f.n, f.m, f.seq_len,
                 (unsigned int)f.status);
    ionic_fault_clear(&f);
    return ok;
}

static bool check_error(const struct fault_case *c, char *why, size_t len)
{
    struct ionic_fault f;
    char err[256] = "";

    if (ionic_fault_parse(c->spec, &f, err, sizeof(err)) == 0) {
        snprintf(why, len, "accepted");
        ionic_fault_clear(&f);
        return false;
    }
    /* A rejected parse owns nothing. */
    if (f.seq != NULL || f.seq_len != 0) {
        snprintf(why, len, "rejected parse left a sequence behind");
        return false;
    }
    if (!strstr(err, c->want_err)) {
        snprintf(why, len, "message '%s'", err);
        return false;
    }
    return true;
}

static bool check_roundtrip(const struct fault_case *c, char *why, size_t len)
{
    struct ionic_fault a, b;
    char err[128] = "";
    static char text[IONIC_FAULT_SEQ_MAX * 8];
    int n;
    bool ok;

    if (ionic_fault_parse(c->spec, &a, err, sizeof(err)) != 0) {
        snprintf(why, len, "spec rejected: %s", err);
        return false;
    }
    n = ionic_fault_format(&a, text, sizeof(text));
    if (n < 0 || (size_t)n >= sizeof(text)) {
        snprintf(why, len, "format returned %d", n);
        ionic_fault_clear(&a);
        return false;
    }
    if (c->want_text && strcmp(text, c->want_text) != 0) {
        snprintf(why, len, "canonical '%.160s'", text);
        ionic_fault_clear(&a);
        return false;
    }
    if (ionic_fault_parse(text, &b, err, sizeof(err)) != 0) {
        snprintf(why, len, "canonical form rejected: %s", err);
        ionic_fault_clear(&a);
        return false;
    }
    ok = same_fault(&a, &b);
    if (!ok)
        snprintf(why, len, "re-parsed fault differs");
    ionic_fault_clear(&a);
    ionic_fault_clear(&b);
    return ok;
}

static bool check_no_fault(char *why, size_t len)
{
    struct ionic_fault_set *set = ionic_fault_set_new();
    bool ok = set != NULL &&
              ionic_fault_set_get(set, IONIC_FAULT_SITE_STATS_VALS) == NULL &&
              ionic_fault_set_get(NULL, IONIC_FAULT_SITE_STATS_VALS) == NULL &&
              ionic_fault_set_get(set, IONIC_FAULT_SITE_COUNT) == NULL;

    for (int i = 0; ok && i < 100; i++)
        if (ionic_fault_event(
                ionic_fault_set_get(set, IONIC_FAULT_SITE_STATS_VALS)))
            ok = false;
    if (!ok)
        snprintf(why, len, "an empty or NULL set produced a fault");
    ionic_fault_set_free(set);
    ionic_fault_set_free(NULL);
    return ok;
}

static bool check_duplicate(const struct fault_case *c, char *why, size_t len)
{
    struct ionic_fault_set *set = ionic_fault_set_new();
    char err[128] = "";
    bool ok = false;

    if (!set) {
        snprintf(why, len, "no set");
        return false;
    }
    if (ionic_fault_set_add(set, c->spec, err, sizeof(err)) != 0)
        snprintf(why, len, "first add rejected: %s", err);
    else if (ionic_fault_set_add(set, c->spec, err, sizeof(err)) == 0)
        snprintf(why, len, "second add accepted");
    else if (!strstr(err, c->want_err))
        snprintf(why, len, "message '%s'", err);
    else
        /* The first fault is still the one installed. */
        ok = ionic_fault_set_get(set, c->site) != NULL;
    ionic_fault_set_free(set);
    return ok;
}

static bool check_truncate(const struct fault_case *c, char *why, size_t len)
{
    struct ionic_fault f;
    char err[128] = "";
    char full[256];
    char small[64];
    int want, got;
    bool ok;

    if (ionic_fault_parse(c->spec, &f, err, sizeof(err)) != 0) {
        snprintf(why, len, "spec rejected: %s", err);
        return false;
    }
    want = ionic_fault_format(&f, full, sizeof(full));
    memset(small, 'Z', sizeof(small));
    got = ionic_fault_format(&f, c->buf_len ? small : NULL, c->buf_len);
    if (c->buf_len == 0)
        ok = got == want;
    else
        ok = got == want && small[c->buf_len - 1] == '\0' &&
             strncmp(small, full, c->buf_len - 1) == 0 &&
             small[c->buf_len] == 'Z';
    if (!ok)
        snprintf(why, len, "returned %d (want %d)", got, want);
    ionic_fault_clear(&f);
    return ok;
}

static bool check_statuses(char *why, size_t len)
{
    /* An independent copy of enum ionic_status_code's names. */
    static const struct {
        const char *name;
        uint8_t code;
    } names[] = {
        {"eversion", 1},  {"eopcode", 2},  {"eio", 3},      {"eperm", 4},
        {"eqid", 5},      {"eqtype", 6},   {"enoent", 7},   {"eintr", 8},
        {"eagain", 9},    {"enomem", 10},  {"efault", 11},  {"ebusy", 12},
        {"eexist", 13},   {"einval", 14},  {"enospc", 15},  {"erange", 16},
        {"bad_addr", 17}, {"dev_cmd", 18}, {"enosupp", 19}, {"error", 29},
        {"erdma", 30},    {"evfid", 31},   {"ebad_fw", 32},
    };

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        struct ionic_fault f;
        char spec[96], text[128], err[128] = "";

        snprintf(spec, sizeof(spec), SITE "every 1 effect fail status %s",
                 names[i].name);
        if (ionic_fault_parse(spec, &f, err, sizeof(err)) != 0) {
            snprintf(why, len, "'%s' rejected: %s", names[i].name, err);
            return false;
        }
        if (f.status != names[i].code ||
            ionic_fault_format(&f, text, sizeof(text)) < 0 ||
            strcmp(text, spec) != 0) {
            snprintf(why, len, "'%s': status %u, canonical '%s'", names[i].name,
                     (unsigned int)f.status, text);
            ionic_fault_clear(&f);
            return false;
        }
        ionic_fault_clear(&f);
    }
    return true;
}

static bool check_sites(char *why, size_t len)
{
    static const char *const specs[] = {
        "eth_tx: every 2 effect drop",
        SITE "every 1 effect fail",
        "eth_rx: every 3 effect nobuf",
    };
    static const enum ionic_fault_site want[] = {
        IONIC_FAULT_SITE_ETH_TX,
        IONIC_FAULT_SITE_STATS_VALS,
        IONIC_FAULT_SITE_ETH_RX,
    };
    struct ionic_fault_set *set = ionic_fault_set_new();
    char err[128] = "";
    bool ok = set != NULL;

    for (size_t i = 0; ok && i < sizeof(specs) / sizeof(specs[0]); i++) {
        const struct ionic_fault *f;

        if (ionic_fault_set_add(set, specs[i], err, sizeof(err)) != 0) {
            snprintf(why, len, "'%s' rejected: %s", specs[i], err);
            ok = false;
            break;
        }
        f = ionic_fault_set_get(set, want[i]);
        if (!f || f->site != want[i]) {
            snprintf(why, len, "'%s' not at its site", specs[i]);
            ok = false;
        }
    }
    /* Events at one site leave the others uncounted. */
    if (ok) {
        ionic_fault_event(ionic_fault_set_get(set, IONIC_FAULT_SITE_ETH_RX));
        if (ionic_fault_set_get(set, IONIC_FAULT_SITE_ETH_TX)->eligible != 0 ||
            ionic_fault_set_get(set, IONIC_FAULT_SITE_STATS_VALS)->eligible !=
                0 ||
            ionic_fault_set_get(set, IONIC_FAULT_SITE_ETH_RX)->eligible != 1) {
            snprintf(why, len, "an eth_rx event was counted at another site");
            ok = false;
        }
    }
    if (set == NULL)
        snprintf(why, len, "no set");
    ionic_fault_set_free(set);
    return ok;
}

static void run_case(const struct fault_case *c)
{
    char why[256] = "";
    bool ok = false;

    switch (c->check) {
    case CHECK_FIRES:
        ok = check_fires(c, why, sizeof(why));
        break;
    case CHECK_FIELDS:
        ok = check_fields(c, why, sizeof(why));
        break;
    case CHECK_ERROR:
        ok = check_error(c, why, sizeof(why));
        break;
    case CHECK_ROUNDTRIP:
        ok = check_roundtrip(c, why, sizeof(why));
        break;
    case CHECK_NO_FAULT:
        ok = check_no_fault(why, sizeof(why));
        break;
    case CHECK_DUPLICATE:
        ok = check_duplicate(c, why, sizeof(why));
        break;
    case CHECK_TRUNCATE:
        ok = check_truncate(c, why, sizeof(why));
        break;
    case CHECK_STATUSES:
        ok = check_statuses(why, sizeof(why));
        break;
    case CHECK_SITES:
        ok = check_sites(why, sizeof(why));
        break;
    default:
        snprintf(why, sizeof(why), "unknown check");
        break;
    }

    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok) {
        printf("     got: %s\n", why);
        failures++;
    }
}

static void check_format_boundaries(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        int capacity; /* -2: needed-1, -1: needed, 0: needed+1; else literal */
    } format_cases[] = {
        {"boundary", "one-byte buffer", "NUL only; canary preserved", 1},
        {"negative", "two bytes short including NUL",
         "truncated prefix and NUL; canary preserved", -2},
        {"boundary", "one byte short including NUL",
         "truncated prefix and NUL; canary preserved", -1},
        {"positive", "exact fit including NUL",
         "full canonical text and NUL; canary preserved", 0},
        {"corner", "oversized buffer", "full text; bytes beyond NUL untouched",
         256},
    };
    struct ionic_fault fault;
    char err[128], full[256];
    int needed;

    if (ionic_fault_parse(SITE "sequence 1,0,1 effect fail status eio", &fault,
                          err, sizeof(err)) != 0) {
        failures++;
        return;
    }
    needed = ionic_fault_format(&fault, full, sizeof(full));
    if (needed <= 1 || (size_t)needed >= sizeof(full)) {
        failures++;
        ionic_fault_clear(&fault);
        return;
    }
    for (size_t row = 0; row < sizeof(format_cases) / sizeof(format_cases[0]);
         row++) {
        char out[257];
        size_t capacity =
            format_cases[row].capacity <= 0
                ? (size_t)(needed + format_cases[row].capacity + 1)
                : (size_t)format_cases[row].capacity;
        size_t written =
            capacity - 1 < (size_t)needed ? capacity - 1 : (size_t)needed;
        bool ok;

        memset(out, 'Z', sizeof(out));
        ok = ionic_fault_format(&fault, out, capacity) == needed &&
             memcmp(out, full, written) == 0 && out[written] == '\0';
        for (size_t j = written + 1; j < sizeof(out); j++)
            ok = ok && out[j] == 'Z';
        printf("%s %s: %s; expected: %s\n", ok ? "ok" : "FAIL",
               format_cases[row].category, format_cases[row].description,
               format_cases[row].expected);
        if (!ok)
            failures++;
    }
    ionic_fault_clear(&fault);
    ionic_fault_clear(&fault);
}

int main(void)
{
    check_format_boundaries();
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_case(&cases[i]);

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
