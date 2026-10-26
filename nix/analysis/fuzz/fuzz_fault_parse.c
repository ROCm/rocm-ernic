/*
 * libFuzzer harness for the fault-injection spec parser
 * (src/ionic/ionic_fault.c).
 *
 * A spec reaches ionic_fault_parse() from --inject on the command line, and
 * later from profile files, so the parser has to hold for any byte string.
 * The harness parses the input as a NUL-terminated string and checks every
 * result against what the parser promises:
 *
 *   - a rejected spec leaves a non-empty message and owns nothing;
 *   - an accepted one is self-consistent: every count it uses is >= 1, a
 *     burst fits its period, a sequence is 1..IONIC_FAULT_SEQ_MAX bytes of
 *     0 or 1, the effect is one the site takes, a fail carries a non-zero
 *     status and any other effect none, and nothing is counted yet;
 *   - its canonical form parses back to the same fault and formats to the
 *     same text again (a fixed point);
 *   - driving it over a run of events, fired counts exactly the events it
 *     fired on and eligible counts every event.
 *
 * A broken promise aborts, which libFuzzer reports as a crash.  The input
 * is copied into an exact-size heap buffer, so ASan catches a read past
 * its terminator.
 *
 * LLVMFuzzerInitialize() first runs a table of seed specs -- positive,
 * negative, boundary and corner -- through the same checks plus whether
 * each should be accepted, so the oracle itself is tested on every run; a
 * failing seed aborts before any fuzzing starts.  With FUZZ_EMIT_SEEDS=1
 * it also writes them into the current directory as a seed corpus.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ionic/ionic_fault.h"

#include "fuzz_harness.h"

/* Events to drive an accepted fault over. */
#define RUN_EVENTS 300

static bool same_fault(const struct ionic_fault *a, const struct ionic_fault *b)
{
    if (a->site != b->site || a->kind != b->kind || a->n != b->n ||
        a->m != b->m || a->effect != b->effect || a->status != b->status ||
        a->seq_len != b->seq_len)
        return false;
    return a->seq_len == 0 || memcmp(a->seq, b->seq, a->seq_len) == 0;
}

/* The effects each site takes, written out apart from the parser's table. */
static bool effect_valid(enum ionic_fault_site site,
                         enum ionic_fault_effect effect)
{
    if (site == IONIC_FAULT_SITE_STATS_VALS)
        return effect == IONIC_FAULT_EFFECT_FAIL;
    if (site == IONIC_FAULT_SITE_ETH_RX)
        return effect == IONIC_FAULT_EFFECT_DROP ||
               effect == IONIC_FAULT_EFFECT_NOBUF;
    if (site == IONIC_FAULT_SITE_ETH_TX)
        return effect == IONIC_FAULT_EFFECT_DROP;
    return false;
}

static const char *check_fields(const struct ionic_fault *f)
{
    if ((unsigned int)f->site >= IONIC_FAULT_SITE_COUNT)
        return "site out of range";
    if (!effect_valid(f->site, f->effect))
        return "effect not valid for the site";
    if (f->effect == IONIC_FAULT_EFFECT_FAIL && f->status == 0)
        return "status 0 is success, not a fault";
    if (f->effect != IONIC_FAULT_EFFECT_FAIL && f->status != 0)
        return "a frame effect carries a status";
    if (f->eligible != 0 || f->fired != 0)
        return "a fresh fault has counted events";
    switch (f->kind) {
    case IONIC_FAULT_PAT_EVERY:
        return f->n >= 1 && f->seq == NULL ? NULL : "bad every";
    case IONIC_FAULT_PAT_AFTER:
        return f->n >= 1 && f->m >= 1 && f->seq == NULL ? NULL : "bad after";
    case IONIC_FAULT_PAT_BURST:
        return f->n >= 1 && f->n <= f->m && f->seq == NULL ? NULL : "bad burst";
    case IONIC_FAULT_PAT_SEQUENCE:
        if (f->seq == NULL || f->seq_len == 0 ||
            f->seq_len > IONIC_FAULT_SEQ_MAX)
            return "bad sequence length";
        for (uint32_t i = 0; i < f->seq_len; i++)
            if (f->seq[i] > 1)
                return "a sequence byte is not 0 or 1";
        return NULL;
    default:
        return "pattern kind out of range";
    }
}

/* Format @f into a fresh heap string, or NULL. */
static char *format_alloc(const struct ionic_fault *f)
{
    int n = ionic_fault_format(f, NULL, 0);
    char *buf;

    if (n < 0)
        return NULL;
    buf = malloc((size_t)n + 1);
    if (buf && ionic_fault_format(f, buf, (size_t)n + 1) != n) {
        free(buf);
        return NULL;
    }
    return buf;
}

static const char *check_roundtrip(const struct ionic_fault *f)
{
    struct ionic_fault g;
    char err[256];
    char *text = format_alloc(f);
    char *again;
    const char *why = NULL;

    if (!text)
        return "canonical form could not be formatted";
    if (ionic_fault_parse(text, &g, err, sizeof(err)) != 0) {
        fprintf(stderr, "fuzz_fault_parse: '%.200s': %s\n", text, err);
        free(text);
        return "canonical form rejected";
    }
    if (!same_fault(f, &g))
        why = "canonical form parses to a different fault";
    again = why ? NULL : format_alloc(&g);
    if (!why && (!again || strcmp(text, again) != 0))
        why = "canonical form is not a fixed point";
    free(again);
    free(text);
    ionic_fault_clear(&g);
    return why;
}

static const char *check_events(struct ionic_fault *f)
{
    uint64_t fires = 0;

    for (uint64_t k = 1; k <= RUN_EVENTS; k++) {
        if (ionic_fault_event(f))
            fires++;
        if (f->eligible != k)
            return "eligible does not count every event";
    }
    return f->fired == fires ? NULL : "fired does not count the fires";
}

/*
 * Parse one spec and check every promise.  Returns NULL when they all
 * hold, else what broke; *accepted says whether the parser took it.
 */
static const char *run_spec(const char *spec, bool *accepted)
{
    struct ionic_fault f;
    char err[256];
    const char *why;

    memset(err, 0, sizeof(err));
    *accepted = ionic_fault_parse(spec, &f, err, sizeof(err)) == 0;
    if (!*accepted) {
        if (err[0] == '\0' || memchr(err, '\0', sizeof(err)) == NULL)
            return "a rejected spec has no message";
        if (f.seq != NULL || f.seq_len != 0)
            return "a rejected spec owns a sequence";
        return NULL;
    }
    why = check_fields(&f);
    if (!why)
        why = check_roundtrip(&f);
    if (!why)
        why = check_events(&f);
    ionic_fault_clear(&f);
    return why;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *spec = malloc(size + 1);
    const char *why;
    bool accepted;

    if (!spec)
        return 0;
    memcpy(spec, data, size);
    spec[size] = '\0';
    why = run_spec(spec, &accepted);
    if (why) {
        fprintf(stderr, "fuzz_fault_parse: %s\n", why);
        abort();
    }
    free(spec);
    return 0;
}

/* ---- Seeds ------------------------------------------------------------- */

struct seed {
    const char *category;
    const char *description;
    const char *expected;
    const char *spec;
    bool want_accepted;
};

static const struct seed seeds[] = {
    {"positive", "every", "accepted", "stats_vals: every 100 effect fail",
     true},
    {"positive", "after with count", "accepted",
     "stats_vals: after 3 count 2 effect fail status enoent", true},
    {"positive", "burst", "accepted",
     "stats_vals: burst 5 every 100 effect fail", true},
    {"positive", "quoted sequence", "accepted",
     "stats_vals: sequence \"1x3,0x10,1\" effect fail status eversion", true},
    {"positive", "canonical form", "accepted",
     "stats_vals: after 3 count 1 effect fail status eio", true},
    {"positive", "eth_rx drop", "accepted", "eth_rx: every 10 effect drop",
     true},
    {"positive", "eth_rx nobuf burst", "accepted",
     "eth_rx: burst 3 every 50 effect nobuf", true},
    {"positive", "eth_tx drop sequence", "accepted",
     "eth_tx: sequence \"1,0x3\" effect drop", true},
    {"negative", "unknown site", "rejected", "foo: every 1 effect fail", false},
    {"negative", "site not yet built", "rejected",
     "rdma_rx: every 1 effect rnr", false},
    {"negative", "fail on an eth site", "rejected",
     "eth_rx: every 1 effect fail", false},
    {"negative", "nobuf on eth_tx", "rejected", "eth_tx: every 1 effect nobuf",
     false},
    {"negative", "status after drop", "rejected",
     "eth_rx: every 1 effect drop status eio", false},
    {"negative", "eth_rx effect not yet built", "rejected",
     "eth_rx: every 1 effect corrupt", false},
    {"negative", "effect not valid for the site", "rejected",
     "stats_vals: every 1 effect drop", false},
    {"negative", "every 0", "rejected", "stats_vals: every 0 effect fail",
     false},
    {"negative", "missing effect", "rejected", "stats_vals: every 5", false},
    {"negative", "trailing token", "rejected",
     "stats_vals: every 5 effect fail junk", false},
    {"negative", "random before E02", "rejected",
     "stats_vals: random 1% effect fail", false},
    {"negative", "number overflow", "rejected",
     "stats_vals: every 18446744073709551616 effect fail", false},
    {"negative", "sequence from a file", "rejected",
     "stats_vals: sequence @trace effect fail", false},
    {"negative", "unclosed quote", "rejected",
     "stats_vals: sequence \"1x3 effect fail", false},
    {"boundary", "every UINT64_MAX", "accepted",
     "stats_vals: every 18446744073709551615 effect fail", true},
    {"boundary", "sequence of exactly IONIC_FAULT_SEQ_MAX", "accepted",
     "stats_vals: sequence 1x65535,0 effect fail", true},
    {"boundary", "sequence one past IONIC_FAULT_SEQ_MAX", "rejected",
     "stats_vals: sequence 1x65536,0 effect fail", false},
    {"boundary", "burst N every N", "accepted",
     "stats_vals: burst 7 every 7 effect fail", true},
    {"corner", "empty spec", "rejected", "", false},
    {"corner", "site and colon only", "rejected", "stats_vals:", false},
    {"corner", "tabs between tokens", "accepted",
     "stats_vals:\tevery\t3\teffect\tfail", true},
    {"corner", "alternating sequence", "accepted",
     "stats_vals: sequence 1,0,1,0,1,0,1x2,0 effect fail", true},
};

/* Seed names are generated locally; environment data never becomes a path. */
static bool emit_seed(size_t i, const uint8_t *buf, size_t len)
{
    char path[32];
    int fd;
    FILE *f;
    bool ok;

    snprintf(path, sizeof(path), "seed-%02zu", i);
    /* Never overwrite an existing file or follow a planted symlink.  With
     * _FORTIFY_SOURCE, glibc 2.40 and later give Clang an
     * open(path, flags, mode) overload, and android-cloexec-open looks for
     * O_CLOEXEC in its mode */
    /* NOLINTBEGIN(android-cloexec-open) */
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    /* NOLINTEND(android-cloexec-open) */
    if (fd < 0) {
        fprintf(stderr, "fuzz_fault_parse: cannot create %s\n", path);
        return false;
    }
    f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        fprintf(stderr, "fuzz_fault_parse: cannot open %s as a stream\n", path);
        return false;
    }
    ok = fwrite(buf, 1, len, f) == len;
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        fprintf(stderr, "fuzz_fault_parse: cannot write %s\n", path);
    return ok;
}

/* libFuzzer gives this its signature */
/* NOLINTNEXTLINE(readability-non-const-parameter) */
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    const char *emit = getenv("FUZZ_EMIT_SEEDS");
    bool emit_seeds = emit && strcmp(emit, "1") == 0;
    int failures = 0;
    int unwritten = 0;

    (void)argc;
    (void)argv;
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        const struct seed *s = &seeds[i];
        bool accepted;
        const char *why = run_spec(s->spec, &accepted);
        bool ok = why == NULL && accepted == s->want_accepted;

        printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", s->category,
               s->description, s->expected);
        if (!ok) {
            printf("     %s%s%s\n", accepted ? "accepted" : "rejected",
                   why ? ": " : "", why ? why : "");
            failures++;
        }
        if (emit_seeds &&
            !emit_seed(i, (const uint8_t *)s->spec, strlen(s->spec)))
            unwritten++;
    }
    fflush(stdout);
    if (failures || unwritten) {
        /* libFuzzer ignores this function's return value, so a broken
         * seed, or a corpus that could not be written, has to stop the run
         * itself.  abort() rather than exit(), which isn't thread safe. */
        if (failures)
            fprintf(stderr, "fuzz_fault_parse: %d seed(s) failed\n", failures);
        if (unwritten)
            fprintf(stderr, "fuzz_fault_parse: %d seed(s) not written\n",
                    unwritten);
        abort();
    }
    return 0;
}
