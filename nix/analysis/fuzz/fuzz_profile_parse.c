/*
 * libFuzzer harness for the NIC profile loader (src/ionic_profile.c).
 *
 * A profile reaches ionic_profile_apply_text() from a --nic-profile file,
 * so the loader has to hold for any byte string.  The harness layers the
 * input onto the compat profile and checks every result against what the
 * loader promises:
 *
 *   - a rejected layer leaves a non-empty message and the profile exactly
 *     as it was;
 *   - an accepted one leaves every value in range: a known ASIC type and
 *     transceiver, a supported speed, an MTU in IONIC_PROFILE_MTU_MIN..MAX,
 *     and a fw_version of 1..31 printable ASCII characters, NUL-terminated;
 *     the chain is untouched (only ionic_profile_apply() extends it);
 *   - its printed form layers back onto compat to the same values and hash,
 *     and prints to the same text again (a fixed point).
 *
 * A broken promise aborts, which libFuzzer reports as a crash.  The input
 * is copied into an exact-size heap buffer with no terminator, so ASan
 * catches a read past its end.
 *
 * LLVMFuzzerInitialize() first runs a table of seed profiles -- positive,
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

#include "ionic_profile.h"

#include "fuzz_harness.h"

/* The IONIC_SPEED_* values link.speed_mbps accepts. */
static const uint32_t speeds[] = {1000,  10000,  25000,  40000,
                                  50000, 100000, 200000, 400000};

static bool same_values(const struct ionic_profile *a,
                        const struct ionic_profile *b)
{
    return a->identity.asic_type == b->identity.asic_type &&
           a->identity.asic_rev == b->identity.asic_rev &&
           strcmp(a->identity.fw_version, b->identity.fw_version) == 0 &&
           a->link.speed_mbps == b->link.speed_mbps &&
           a->link.xcvr == b->link.xcvr && a->link.mtu_max == b->link.mtu_max;
}

static const char *check_fields(const struct ionic_profile *p)
{
    const char *fw = p->identity.fw_version;
    size_t n = strnlen(fw, sizeof(p->identity.fw_version));
    bool speed_ok = false;

    if (p->identity.asic_type > IONIC_PROFILE_ASIC_SALINA)
        return "asic_type out of range";
    if (p->link.xcvr > IONIC_PROFILE_XCVR_NONE)
        return "xcvr out of range";
    for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++)
        speed_ok = speed_ok || p->link.speed_mbps == speeds[i];
    if (!speed_ok)
        return "speed_mbps is not a supported speed";
    if (p->link.mtu_max < IONIC_PROFILE_MTU_MIN ||
        p->link.mtu_max > IONIC_PROFILE_MTU_MAX)
        return "mtu_max out of range";
    if (n == 0 || n > IONIC_PROFILE_FW_VERSION_MAX)
        return "fw_version empty or not NUL-terminated";
    for (size_t i = 0; i < n; i++)
        if (fw[i] < 0x20 || fw[i] > 0x7e)
            return "fw_version is not printable ASCII";
    if (strcmp(p->chain, "compat") != 0)
        return "a text layer changed the chain";
    return NULL;
}

/* Format @p into a fresh heap string, or NULL. */
static char *format_alloc(const struct ionic_profile *p, int *len)
{
    int n = ionic_profile_format(p, NULL, 0);
    char *buf;

    if (n < 0)
        return NULL;
    buf = malloc((size_t)n + 1);
    if (buf && ionic_profile_format(p, buf, (size_t)n + 1) != n) {
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

static const char *check_roundtrip(const struct ionic_profile *p)
{
    struct ionic_profile q;
    char err[512];
    int n, m;
    char *text = format_alloc(p, &n);
    char *again;
    const char *why = NULL;

    if (!text)
        return "printed form could not be formatted";
    ionic_profile_defaults(&q);
    if (ionic_profile_apply_text(&q, "printed", text, (size_t)n, err,
                                 sizeof(err)) != 0) {
        fprintf(stderr, "fuzz_profile_parse: %s\n", err);
        free(text);
        return "printed form rejected";
    }
    if (!same_values(p, &q))
        why = "printed form reads back to different values";
    else if (ionic_profile_hash(p) != ionic_profile_hash(&q))
        why = "printed form reads back to a different hash";
    again = why ? NULL : format_alloc(&q, &m);
    if (!why && (!again || strcmp(text, again) != 0))
        why = "printed form is not a fixed point";
    free(again);
    free(text);
    return why;
}

/*
 * Layer @len bytes of @text onto compat and check every promise.  Returns
 * NULL when they all hold, else what broke; *accepted says whether the
 * loader took it.
 */
static const char *run_profile(const char *text, size_t len, bool *accepted)
{
    struct ionic_profile p, before;
    char err[512];
    const char *why;

    ionic_profile_defaults(&p);
    before = p;
    memset(err, 0, sizeof(err));
    *accepted =
        ionic_profile_apply_text(&p, "fuzz", text, len, err, sizeof(err)) == 0;
    if (!*accepted) {
        if (err[0] == '\0' || memchr(err, '\0', sizeof(err)) == NULL)
            return "a rejected layer has no message";
        if (!same_values(&p, &before) || strcmp(p.chain, before.chain) != 0)
            return "a rejected layer changed the profile";
        return NULL;
    }
    why = check_fields(&p);
    if (!why)
        why = check_roundtrip(&p);
    return why;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *text = malloc(size ? size : 1);
    const char *why;
    bool accepted;

    if (!text)
        return 0;
    if (size)
        memcpy(text, data, size);
    why = run_profile(text, size, &accepted);
    if (why) {
        fprintf(stderr, "fuzz_profile_parse: %s\n", why);
        abort();
    }
    free(text);
    return 0;
}

/* ---- Seeds ------------------------------------------------------------- */

struct seed {
    const char *category;
    const char *description;
    const char *expected;
    const char *text; /* NULL: the built-in profile named in description */
    bool want_accepted;
};

static const struct seed seeds[] = {
    {"positive", "compat", "accepted", NULL, true},
    {"positive", "small-vm", "accepted", NULL, true},
    {"positive", "every key at once", "accepted",
     "[identity]\nasic_type = elba\nasic_rev = 3\nfw_version = 1.8.0-E-32\n"
     "[link]\nspeed_mbps = 400000\nxcvr = fiber\nmtu_max = 9000\n",
     true},
    {"positive", "comments and blank lines", "accepted",
     "# a profile\n\n[link]\n# fast\nspeed_mbps = 200000\n", true},
    {"negative", "unknown section", "rejected", "[quues]\neth = 64\n", false},
    {"negative", "unknown key", "rejected", "[link]\nsped_mbps = 1000\n",
     false},
    {"negative", "unsupported speed", "rejected",
     "[link]\nspeed_mbps = 12345\n", false},
    {"negative", "not an integer", "rejected", "[link]\nmtu_max = big\n",
     false},
    {"negative", "unknown asic", "rejected", "[identity]\nasic_type = zen\n",
     false},
    {"negative", "key outside a section", "rejected", "speed_mbps = 1000\n",
     false},
    {"negative", "duplicate key", "rejected",
     "[link]\nxcvr = fiber\nxcvr = copper\n", false},
    {"negative", "a good key, then a bad value", "rejected",
     "[link]\nspeed_mbps = 40000\nmtu_max = 1\n", false},
    {"negative", "fw_version with a backslash", "rejected",
     "[identity]\nfw_version = a\\\\b\n", false},
    {"boundary", "mtu_max at its minimum", "accepted",
     "[link]\nmtu_max = 1500\n", true},
    {"boundary", "mtu_max at its maximum", "accepted",
     "[link]\nmtu_max = 9582\n", true},
    {"boundary", "mtu_max one past its maximum", "rejected",
     "[link]\nmtu_max = 9583\n", false},
    {"boundary", "stats.interval_ms at its minimum", "accepted",
     "[stats]\ninterval_ms = 10\n", true},
    {"boundary", "stats.interval_ms at its maximum", "accepted",
     "[stats]\ninterval_ms = 60000\n", true},
    {"boundary", "stats.interval_ms one below its minimum", "rejected",
     "[stats]\ninterval_ms = 9\n", false},
    {"boundary", "stats.interval_ms one past its maximum", "rejected",
     "[stats]\ninterval_ms = 60001\n", false},
    {"boundary", "asic_rev 255", "accepted", "[identity]\nasic_rev = 255\n",
     true},
    {"boundary", "asic_rev 256", "rejected", "[identity]\nasic_rev = 256\n",
     false},
    {"boundary", "fw_version of 31 characters", "accepted",
     "[identity]\nfw_version = 1234567890123456789012345678901\n", true},
    {"boundary", "fw_version of 32 characters", "rejected",
     "[identity]\nfw_version = 12345678901234567890123456789012\n", false},
    {"corner", "empty profile", "accepted", "", true},
    {"corner", "CRLF line endings", "accepted",
     "[link]\r\nspeed_mbps = 50000\r\n", true},
    {"corner", "no newline at the end", "accepted",
     "[link]\nspeed_mbps = 50000", true},
    {"corner", "an empty section name", "rejected", "[]\n", false},
    {"corner", "a \\s escape in fw_version", "accepted, as a space",
     "[identity]\nfw_version = a\\sb\n", true},
    {"corner", "a non-ASCII fw_version", "rejected",
     "[identity]\nfw_version = v1\xc3\xa9\n", false},
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
        fprintf(stderr, "fuzz_profile_parse: cannot create %s\n", path);
        return false;
    }
    f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        fprintf(stderr, "fuzz_profile_parse: cannot open %s as a stream\n",
                path);
        return false;
    }
    ok = fwrite(buf, 1, len, f) == len;
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        fprintf(stderr, "fuzz_profile_parse: cannot write %s\n", path);
    return ok;
}

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
        const char *text =
            s->text ? s->text : ionic_profile_builtin(s->description);
        bool accepted = false;
        const char *why =
            text ? run_profile(text, strlen(text), &accepted) : "no text";
        bool ok = why == NULL && accepted == s->want_accepted;

        printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", s->category,
               s->description, s->expected);
        if (!ok) {
            printf("     %s%s%s\n", accepted ? "accepted" : "rejected",
                   why ? ": " : "", why ? why : "");
            failures++;
        }
        if (emit_seeds && text &&
            !emit_seed(i, (const uint8_t *)text, strlen(text)))
            unwritten++;
    }
    fflush(stdout);
    if (failures || unwritten) {
        /* libFuzzer ignores this function's return value, so a broken
         * seed, or a corpus that could not be written, has to stop the run
         * itself.  abort() rather than exit(), which isn't thread safe. */
        if (failures)
            fprintf(stderr, "fuzz_profile_parse: %d seed(s) failed\n",
                    failures);
        if (unwritten)
            fprintf(stderr, "fuzz_profile_parse: %d seed(s) not written\n",
                    unwritten);
        abort();
    }
    return 0;
}
