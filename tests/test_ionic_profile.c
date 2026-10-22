/*
 * Unit tests for the NIC profile loader (src/ionic_profile.c).
 *
 * A profile is ini text layered onto the compat profile key by key.  The
 * loader has to be strict -- an unknown section or key, a bad type or a
 * value out of range is an error naming the source, line and key -- and
 * atomic: a layer that fails changes nothing.  Every row here therefore
 * checks the profile after its last layer, error or not.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ionic_profile.h"

static int failures;

/* What a profile should hold; compat unless a row says otherwise. */
struct want {
    uint8_t asic_type;
    uint8_t asic_rev;
    const char *fw_version;
    uint32_t speed_mbps;
    uint8_t xcvr;
    uint32_t mtu_max;
    uint32_t stats_interval_ms;
    uint32_t eth;
    uint32_t rdma_eqs;
    uint32_t intr_vectors; /* 0 is auto */
};

/* The fields of a struct want for compat.  Braced at each use: a macro whose
 * body is a braced list formats differently in clang-format 18.1.3 and
 * 18.1.8. */
#define COMPAT_VALUES 0, 0, "rocm-ernic-1.0", 100000, 0, 9198, 1000, 4, 32, 32

static bool profile_is(const struct ionic_profile *p, const struct want *w)
{
    return p->identity.asic_type == w->asic_type &&
           p->identity.asic_rev == w->asic_rev &&
           strcmp(p->identity.fw_version, w->fw_version) == 0 &&
           p->link.speed_mbps == w->speed_mbps && p->link.xcvr == w->xcvr &&
           p->link.mtu_max == w->mtu_max &&
           p->stats.interval_ms == w->stats_interval_ms &&
           p->queues.eth == w->eth && p->queues.rdma_eqs == w->rdma_eqs &&
           p->queues.intr_vectors == w->intr_vectors;
}

static void print_profile(const char *label, const struct ionic_profile *p)
{
    printf("     %s: asic_type %u, asic_rev %u, fw_version '%s', speed %" PRIu32
           ", xcvr %u, mtu_max %" PRIu32 ", stats interval %" PRIu32
           ", queues eth %" PRIu32 " rdma_eqs %" PRIu32 " intr_vectors %" PRIu32
           "\n",
           label, p->identity.asic_type, p->identity.asic_rev,
           p->identity.fw_version, p->link.speed_mbps, p->link.xcvr,
           p->link.mtu_max, p->stats.interval_ms, p->queues.eth,
           p->queues.rdma_eqs, p->queues.intr_vectors);
}

/* ---- Layered text -------------------------------------------------------- */

#define MAX_LAYERS 3

/*
 * Up to three ini layers applied in turn onto compat, as "layer1".."layer3".
 * The first layer that fails stops the row; its error must contain
 * @want_err (NULL: every layer must succeed).  Either way the profile must
 * then hold @want.
 */
struct text_case {
    const char *category;
    const char *description;
    const char *expected;
    const char *layers[MAX_LAYERS];
    const char *want_err;
    struct want want;
};

static const struct text_case text_cases[] = {
    /* positive */
    {"positive",
     "a key that changes the link speed",
     "speed 400000; every other key as compat",
     {"[link]\nspeed_mbps = 400000\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 400000, 0, 9198, 1000, 4, 32, 32}},
    {"positive",
     "every key set at once",
     "each value as written",
     {"[identity]\nasic_type = salina\nasic_rev = 7\nfw_version = 2.0.1\n"
      "[link]\nspeed_mbps = 200000\nxcvr = fiber\nmtu_max = 4000\n"},
     NULL,
     {4, 7, "2.0.1", 200000, 1, 4000, 1000, 4, 32, 32}},
    {"positive",
     "three layers, each setting some keys",
     "each key from the last layer that set it",
     {"[identity]\nfw_version = one\n[link]\nspeed_mbps = 25000\n",
      "[link]\nspeed_mbps = 40000\n", "[link]\nxcvr = none\n"},
     NULL,
     {0, 0, "one", 40000, 2, 9198, 1000, 4, 32, 32}},
    {"positive",
     "asic_type capri",
     "asic_type 1",
     {"[identity]\nasic_type = capri\n"},
     NULL,
     {1, 0, "rocm-ernic-1.0", 100000, 0, 9198, 1000, 4, 32, 32}},
    {"positive",
     "asic_type giglio",
     "asic_type 3",
     {"[identity]\nasic_type = giglio\n"},
     NULL,
     {3, 0, "rocm-ernic-1.0", 100000, 0, 9198, 1000, 4, 32, 32}},
    {"positive",
     "comments, blank lines, indentation and spaces around '='",
     "accepted; the values as written",
     {"# a comment\n\n  [link]  \n\t# another\n  speed_mbps=10000\n"
      "xcvr   =   fiber\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 10000, 1, 9198, 1000, 4, 32, 32}},
    {"positive",
     "a fw_version with inner spaces and punctuation",
     "the string as written",
     {"[identity]\nfw_version = 1.8.0 (E-32) #7\n"},
     NULL,
     {0, 0, "1.8.0 (E-32) #7", 100000, 0, 9198, 1000, 4, 32, 32}},

    /* negative */
    {"negative",
     "an unknown section [quues]",
     "error naming the line and the section",
     {"# c\n[quues]\neth = 64\n"},
     "layer1:2: unknown section [quues] (known sections: identity, link, "
     "queues, stats)",
     {COMPAT_VALUES}},
    {"negative",
     "a section a later PR adds, [rdma]",
     "rejected as unknown in this build",
     {"[rdma]\nstats = none\n"},
     "layer1:1: unknown section [rdma]",
     {COMPAT_VALUES}},
    {"negative",
     "a mistyped key, link.sped_mbps",
     "error naming the line and the key; nothing changes",
     {"[link]\nxcvr = fiber\nsped_mbps = 400000\n"},
     "layer1:3: unknown key 'link.sped_mbps'",
     {COMPAT_VALUES}},
    {"negative",
     "a speed that is not an IONIC_SPEED_* value",
     "error listing the supported speeds",
     {"[link]\nspeed_mbps = 12345\n"},
     "layer1:2: link.speed_mbps: '12345' is not a supported speed (1000, "
     "10000, 25000, 40000, 50000, 100000, 200000 or 400000 Mbps)",
     {COMPAT_VALUES}},
    {"negative",
     "mtu_max = big",
     "error naming the key and the type",
     {"[link]\nmtu_max = big\n"},
     "layer1:2: link.mtu_max: integer expected, got 'big'",
     {COMPAT_VALUES}},
    {"negative",
     "an asic_type that does not exist",
     "error listing the ASIC types",
     {"[identity]\nasic_type = vega\n"},
     "layer1:2: identity.asic_type: 'vega' is not one of none, capri, elba, "
     "giglio, salina",
     {COMPAT_VALUES}},
    {"negative",
     "an xcvr that does not exist",
     "error listing the choices",
     {"[link]\nxcvr = dac\n"},
     "layer1:2: link.xcvr: 'dac' is not one of copper, fiber, none",
     {COMPAT_VALUES}},
    {"negative",
     "a fw_version of 32 characters",
     "error giving the length and the limit",
     {"[identity]\nfw_version = abcdefghijklmnopqrstuvwxyz012345\n"},
     "layer1:2: identity.fw_version: 32 characters; at most 31",
     {COMPAT_VALUES}},
    {"negative",
     "a fw_version with a backslash escape",
     "rejected: a backslash can't round-trip",
     {"[identity]\nfw_version = a\\\\b\n"},
     "identity.fw_version: must be 1 to 31 printable ASCII characters",
     {COMPAT_VALUES}},
    {"negative",
     "a key before any section",
     "error naming the line and the key",
     {"speed_mbps = 1000\n"},
     "layer1:1: key 'speed_mbps' is outside any [section]",
     {COMPAT_VALUES}},
    {"negative",
     "a key set twice in one layer",
     "error naming both lines, not the last one silently winning",
     {"[link]\nspeed_mbps = 1000\nspeed_mbps = 10000\n"},
     "layer1:3: key 'link.speed_mbps' is set twice (first at line 2)",
     {COMPAT_VALUES}},
    {"negative",
     "a section repeated in one layer",
     "error naming both lines",
     {"[link]\nxcvr = fiber\n[identity]\n[link]\n"},
     "layer1:4: section [link] appears twice (first at line 1)",
     {COMPAT_VALUES}},
    {"negative",
     "a line that is neither a section nor a key",
     "error naming the line",
     {"[link]\nfast please\n"},
     "layer1:2: expected '[section]', 'key = value' or a '#' comment",
     {COMPAT_VALUES}},
    {"negative",
     "a GKeyFile locale key, fw_version[de]",
     "rejected as a syntax error, not as a separate key",
     {"[identity]\nfw_version[de] = x\n"},
     "layer1:2: expected '[section]', 'key = value'",
     {COMPAT_VALUES}},
    {"negative",
     "a negative speed",
     "integer expected",
     {"[link]\nspeed_mbps = -1\n"},
     "layer1:2: link.speed_mbps: integer expected, got '-1'",
     {COMPAT_VALUES}},
    {"negative",
     "a speed with a '+' sign",
     "integer expected",
     {"[link]\nspeed_mbps = +100000\n"},
     "integer expected, got '+100000'",
     {COMPAT_VALUES}},
    {"negative",
     "a fw_version that is not ASCII",
     "rejected; nothing changes",
     {"[identity]\nfw_version = v1\xc3\xa9\n"},
     "layer1",
     {COMPAT_VALUES}},
    {"negative",
     "an empty section name '[]'",
     "error naming the line",
     {"[]\n"},
     "layer1:1: a section header is '[name]'",
     {COMPAT_VALUES}},

    /* boundary */
    {"boundary",
     "asic_rev 0 and fw_version of 1 character",
     "accepted",
     {"[identity]\nasic_rev = 0\nfw_version = x\n"},
     NULL,
     {0, 0, "x", 100000, 0, 9198, 1000, 4, 32, 32}},
    {"boundary",
     "asic_rev 255",
     "accepted",
     {"[identity]\nasic_rev = 255\n"},
     NULL,
     {0, 255, "rocm-ernic-1.0", 100000, 0, 9198, 1000, 4, 32, 32}},
    {"boundary",
     "asic_rev 256",
     "out of range 0..255",
     {"[identity]\nasic_rev = 256\n"},
     "layer1:2: identity.asic_rev: '256' out of range 0..255",
     {COMPAT_VALUES}},
    {"boundary",
     "speed 1000, the slowest",
     "accepted",
     {"[link]\nspeed_mbps = 1000\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 1000, 0, 9198, 1000, 4, 32, 32}},
    {"boundary",
     "speed 999, one below",
     "not a supported speed",
     {"[link]\nspeed_mbps = 999\n"},
     "'999' is not a supported speed",
     {COMPAT_VALUES}},
    {"boundary",
     "speed 400001, one above the fastest",
     "not a supported speed",
     {"[link]\nspeed_mbps = 400001\n"},
     "'400001' is not a supported speed",
     {COMPAT_VALUES}},
    {"boundary",
     "mtu_max 1500 and 9582 in two layers",
     "the last, 9582",
     {"[link]\nmtu_max = 1500\n", "[link]\nmtu_max = 9582\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 100000, 0, 9582, 1000, 4, 32, 32}},
    {"boundary",
     "mtu_max 1499",
     "out of range 1500..9582",
     {"[link]\nmtu_max = 1499\n"},
     "layer1:2: link.mtu_max: '1499' out of range 1500..9582",
     {COMPAT_VALUES}},
    {"boundary",
     "mtu_max 9583",
     "out of range 1500..9582",
     {"[link]\nmtu_max = 9583\n"},
     "'9583' out of range 1500..9582",
     {COMPAT_VALUES}},
    {"boundary",
     "mtu_max one past UINT64_MAX",
     "out of range, no wrap",
     {"[link]\nmtu_max = 18446744073709551616\n"},
     "'18446744073709551616' out of range",
     {COMPAT_VALUES}},
    {"boundary",
     "an empty fw_version",
     "rejected",
     {"[identity]\nfw_version =\n"},
     "identity.fw_version: must be 1 to 31",
     {COMPAT_VALUES}},
    {"boundary",
     "an empty layer",
     "no key changes",
     {""},
     NULL,
     {COMPAT_VALUES}},
    {"positive",
     "stats.interval_ms = 100",
     "interval 100; every other key as compat",
     {"[stats]\ninterval_ms = 100\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 100000, 0, 9198, 100, 4, 32, 32}},
    {"boundary",
     "stats.interval_ms 10 and 60000 in two layers",
     "the last, 60000",
     {"[stats]\ninterval_ms = 10\n", "[stats]\ninterval_ms = 60000\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 100000, 0, 9198, 60000, 4, 32, 32}},
    {"boundary",
     "stats.interval_ms 9",
     "out of range 10..60000 ms",
     {"[stats]\ninterval_ms = 9\n"},
     "layer1:2: stats.interval_ms: '9' out of range 10..60000 ms",
     {COMPAT_VALUES}},
    {"boundary",
     "stats.interval_ms 60001",
     "out of range 10..60000 ms",
     {"[stats]\ninterval_ms = 60001\n"},
     "'60001' out of range 10..60000 ms",
     {COMPAT_VALUES}},
    {"negative",
     "stats.interval_ms = 0",
     "out of range: a zero interval would publish on every loop",
     {"[stats]\ninterval_ms = 0\n"},
     "'0' out of range 10..60000 ms",
     {COMPAT_VALUES}},
    {"negative",
     "stats.interval_ms = fast",
     "integer expected",
     {"[stats]\ninterval_ms = fast\n"},
     "stats.interval_ms: integer expected, got 'fast'",
     {COMPAT_VALUES}},
    {"negative",
     "an unknown key in [stats]",
     "rejected, naming the key",
     {"[stats]\ninterval = 100\n"},
     "layer1:2: unknown key 'stats.interval'",
     {COMPAT_VALUES}},

    /* queues */
    {"positive",
     "queues.eth = 16",
     "eth 16; every other key as compat",
     {"[queues]\neth = 16\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 100000, 0, 9198, 1000, 16, 32, 32}},
    {"positive",
     "queues.intr_vectors = auto",
     "stored as auto (0)",
     {"[queues]\nintr_vectors = auto\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 100000, 0, 9198, 1000, 4, 32, 0}},
    {"positive",
     "every [queues] key at once",
     "each value as written",
     {"[queues]\neth = 8\nrdma_eqs = 16\nintr_vectors = 40\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 100000, 0, 9198, 1000, 8, 16, 40}},
    {"negative",
     "queues.eth = many",
     "integer expected",
     {"[queues]\neth = many\n"},
     "queues.eth: integer expected, got 'many'",
     {COMPAT_VALUES}},
    {"negative",
     "queues.intr_vectors = lots",
     "auto or an integer expected",
     {"[queues]\nintr_vectors = lots\n"},
     "queues.intr_vectors: 'auto' or an integer expected, got 'lots'",
     {COMPAT_VALUES}},
    {"negative",
     "an unknown key in [queues]",
     "rejected, naming the key",
     {"[queues]\nrx = 4\n"},
     "layer1:2: unknown key 'queues.rx'",
     {COMPAT_VALUES}},
    {"boundary",
     "queues.eth 1 and 64 in two layers",
     "the last, 64",
     {"[queues]\neth = 1\n", "[queues]\neth = 64\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 100000, 0, 9198, 1000, 64, 32, 32}},
    {"boundary",
     "queues.eth 0",
     "out of range 1..64",
     {"[queues]\neth = 0\n"},
     "layer1:2: queues.eth: '0' out of range 1..64",
     {COMPAT_VALUES}},
    {"boundary",
     "queues.eth 65",
     "out of range 1..64",
     {"[queues]\neth = 65\n"},
     "'65' out of range 1..64",
     {COMPAT_VALUES}},
    {"boundary",
     "queues.rdma_eqs 4",
     "accepted: the fewest the RDMA driver takes",
     {"[queues]\nrdma_eqs = 4\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 100000, 0, 9198, 1000, 4, 4, 32}},
    {"boundary",
     "queues.rdma_eqs 3",
     "out of range 4..32: the RDMA driver needs 4",
     {"[queues]\nrdma_eqs = 3\n"},
     "queues.rdma_eqs: '3' out of range 4..32",
     {COMPAT_VALUES}},
    {"boundary",
     "queues.rdma_eqs 33",
     "out of range 4..32",
     {"[queues]\nrdma_eqs = 33\n"},
     "'33' out of range 4..32",
     {COMPAT_VALUES}},
    {"boundary",
     "queues.intr_vectors 2",
     "accepted",
     {"[queues]\nintr_vectors = 2\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 100000, 0, 9198, 1000, 4, 32, 2}},
    {"boundary",
     "queues.intr_vectors 128",
     "accepted",
     {"[queues]\nintr_vectors = 128\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 100000, 0, 9198, 1000, 4, 32, 128}},
    {"boundary",
     "queues.intr_vectors 1",
     "out of range 2..128",
     {"[queues]\nintr_vectors = 1\n"},
     "queues.intr_vectors: '1' out of range 2..128",
     {COMPAT_VALUES}},
    {"boundary",
     "queues.intr_vectors 129",
     "out of range 2..128",
     {"[queues]\nintr_vectors = 129\n"},
     "'129' out of range 2..128",
     {COMPAT_VALUES}},
    {"corner",
     "queues.intr_vectors = AUTO",
     "rejected: values are case sensitive",
     {"[queues]\nintr_vectors = AUTO\n"},
     "'auto' or an integer expected, got 'AUTO'",
     {COMPAT_VALUES}},
    {"corner",
     "queues.intr_vectors auto, then a number in a later layer",
     "the number",
     {"[queues]\nintr_vectors = auto\n", "[queues]\nintr_vectors = 64\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 100000, 0, 9198, 1000, 4, 32, 64}},
    {"corner",
     "queues.eth = 18446744073709551616",
     "out of range, not wrapped",
     {"[queues]\neth = 18446744073709551616\n"},
     "'18446744073709551616' out of range 1..64",
     {COMPAT_VALUES}},

    /* corner */
    {"corner",
     "a layer that fails after a good one",
     "the good layer stays; nothing from the failed one, not even the "
     "two valid keys set before its bad value",
     {"[link]\nspeed_mbps = 25000\n",
      "[link]\nspeed_mbps = 40000\nxcvr = fiber\nmtu_max = 1\n"},
     "layer2:4: link.mtu_max: '1' out of range 1500..9582",
     {0, 0, "rocm-ernic-1.0", 25000, 0, 9198, 1000, 4, 32, 32}},
    {"corner",
     "a \\s escape in fw_version",
     "decoded to a space, as GKeyFile does",
     {"[identity]\nfw_version = a\\sb\n"},
     NULL,
     {0, 0, "a b", 100000, 0, 9198, 1000, 4, 32, 32}},
    {"corner",
     "trailing blanks after a value",
     "ignored, as invisible",
     {"[link]\nspeed_mbps = 10000  \t\nxcvr = none \n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 10000, 2, 9198, 1000, 4, 32, 32}},
    {"corner",
     "CRLF line endings",
     "accepted",
     {"[link]\r\nspeed_mbps = 50000\r\n"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 50000, 0, 9198, 1000, 4, 32, 32}},
    {"corner",
     "no newline at the end",
     "the last line still counts",
     {"[link]\nspeed_mbps = 50000"},
     NULL,
     {0, 0, "rocm-ernic-1.0", 50000, 0, 9198, 1000, 4, 32, 32}},
    {"corner",
     "a section with no keys",
     "accepted; nothing changes",
     {"[identity]\n[link]\n"},
     NULL,
     {COMPAT_VALUES}},
    {"corner",
     "the same key in two layers",
     "allowed: the later layer wins",
     {"[link]\nxcvr = fiber\n", "[link]\nxcvr = copper\n"},
     NULL,
     {COMPAT_VALUES}},
};

static void run_text_case(const struct text_case *c)
{
    struct ionic_profile p;
    char err[512] = "";
    const char *failed_at = NULL;
    bool unchanged = true;
    bool ok;

    ionic_profile_defaults(&p);
    for (int i = 0; i < MAX_LAYERS && c->layers[i]; i++) {
        unsigned char before[sizeof(p)];
        char source[16];
        size_t len = strlen(c->layers[i]);

        memcpy(before, &p, sizeof(p));
        snprintf(source, sizeof(source), "layer%d", i + 1);
        if (ionic_profile_apply_text(&p, source, c->layers[i], len, err,
                                     sizeof(err)) != 0) {
            unchanged = memcmp(before, &p, sizeof(p)) == 0;
            failed_at = c->layers[i];
            break;
        }
    }
    ok = unchanged &&
         (c->want_err ? failed_at && strstr(err, c->want_err) : !failed_at) &&
         profile_is(&p, &c->want) && strcmp(p.chain, "compat") == 0;

    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok) {
        printf("     error: %s\n", failed_at ? err : "(none)");
        if (c->want_err)
            printf("     wanted an error containing: %s\n", c->want_err);
        print_profile("got", &p);
        failures++;
    }
}

/* ---- Profiles by name or path ------------------------------------------ */

/* The scratch directory.  Always under /tmp, not $TMPDIR: a path built from
 * the environment is what CodeQL's cpp/path-injection reports. */
static char g_tmpdir[] = "/tmp/ernic-profile-XXXXXX";

/* Private, exclusive files even with a permissive caller umask. */
static FILE *scratch_open(const char *path)
{
    /* With _FORTIFY_SOURCE, glibc 2.40 and later give Clang an
     * open(path, flags, mode) overload, and android-cloexec-open looks for
     * O_CLOEXEC in its mode */
    /* NOLINTBEGIN(android-cloexec-open) */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    /* NOLINTEND(android-cloexec-open) */
    FILE *f;

    if (fd < 0)
        return NULL;
    f = fdopen(fd, "wb");
    if (!f)
        close(fd);
    return f;
}

/* Write @text to @name in the scratch directory; returns its path, or NULL
 * after printing why. */
static const char *scratch_file(const char *name, const char *text)
{
    static char path[512];
    FILE *f;
    bool ok;

    snprintf(path, sizeof(path), "%s/%s", g_tmpdir, name);
    f = scratch_open(path);
    if (!f) {
        printf("     setup: cannot create %s\n", path);
        return NULL;
    }
    ok = fputs(text, f) >= 0;
    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        printf("     setup: cannot write %s\n", path);
        return NULL;
    }
    return path;
}

struct spec_case {
    const char *category;
    const char *description;
    const char *expected;
    const char *spec;     /* a name, a path, or "@file" for the row's file */
    const char *file;     /* text written to a scratch file, if spec is @ */
    const char *want_err; /* substring, or NULL for success */
    const char *want_chain;
    uint32_t want_speed;
};

static const struct spec_case spec_cases[] = {
    {"positive", "built-in compat by name",
     "accepted; the chain names it twice, base and layer", "compat", NULL, NULL,
     "compat + compat", 100000},
    {"positive", "built-in small-vm by name", "accepted; the link as compat",
     "small-vm", NULL, NULL, "compat + small-vm", 100000},
    {"positive", "built-in generic-100g by name", "accepted; 100G",
     "generic-100g", NULL, NULL, "compat + generic-100g", 100000},
    {"positive", "built-in generic-400g by name", "accepted; 400G",
     "generic-400g", NULL, NULL, "compat + generic-400g", 400000},
    {"positive", "a file by path", "its keys applied; the path in the chain",
     "@fast.ini", "[link]\nspeed_mbps = 400000\n", NULL, "compat + @fast.ini",
     400000},
    {"negative", "an unknown built-in name",
     "error listing the built-in profiles and how to name a file", "nope", NULL,
     "unknown NIC profile 'nope' (built-in profiles: compat, small-vm, "
     "generic-100g, generic-400g; a "
     "file path needs a '/' or an .ini suffix)",
     "compat", 100000},
    {"negative", "a missing file", "error naming the path and the errno text",
     "./nope.ini", NULL,
     "cannot open NIC profile './nope.ini': No such file or directory",
     "compat", 100000},
    {"negative", "a directory", "not a regular file", "@", NULL,
     "is not a regular file", "compat", 100000},
    {"negative", "a file with a bad key",
     "error naming the file's path and line", "@bad.ini",
     "[link]\n\nspeedmbps = 1\n", "bad.ini:3: unknown key 'link.speedmbps'",
     "compat", 100000},
    {"boundary", "an empty name", "rejected", "", NULL,
     "empty NIC profile name", "compat", 100000},
    {"corner", "a bare name ending in .ini", "read as a file, not a built-in",
     "compat.ini", NULL, "cannot open NIC profile 'compat.ini'", "compat",
     100000},
};

static void run_spec_case(const struct spec_case *c)
{
    struct ionic_profile p;
    char err[512] = "";
    char want_chain[600];
    const char *spec = c->spec;
    int rc;
    bool ok;

    if (spec[0] == '@') {
        spec = c->file ? scratch_file(spec + 1, c->file) : g_tmpdir;
        if (!spec) {
            printf("FAIL %s: %s; expected: %s\n", c->category, c->description,
                   c->expected);
            failures++;
            return;
        }
        if (strncmp(c->want_chain, "compat + @", 10) == 0)
            snprintf(want_chain, sizeof(want_chain), "compat + %s", spec);
        else
            snprintf(want_chain, sizeof(want_chain), "%s", c->want_chain);
    } else {
        snprintf(want_chain, sizeof(want_chain), "%s", c->want_chain);
    }
    ionic_profile_defaults(&p);
    rc = ionic_profile_apply(&p, spec, err, sizeof(err));
    ok = (c->want_err ? rc != 0 && strstr(err, c->want_err) : rc == 0) &&
         strcmp(p.chain, want_chain) == 0 && p.link.speed_mbps == c->want_speed;

    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok) {
        printf(
            "     rc %d, error: %s\n     chain '%s' (want '%s'), speed %" PRIu32
            "\n",
            rc, rc ? err : "(none)", p.chain, want_chain, p.link.speed_mbps);
        failures++;
    }
}

/* ---- Properties -------------------------------------------------------- */

static void report(bool ok, const char *category, const char *description,
                   const char *expected)
{
    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", category,
           description, expected);
    if (!ok)
        failures++;
}

/* The built-in text of @name is byte-identical to profiles/@name.ini. */
static void check_builtin_matches_file(const char *name)
{
    char path[512], desc[128];
    const char *text = ionic_profile_builtin(name);
    char *file = NULL;
    FILE *f;
    bool ok = false;

    snprintf(path, sizeof(path), "%s/%s.ini", ERNIC_PROFILES_DIR, name);
    f = fopen(path, "rbe");
    if (f && text) {
        size_t len = 0;

        file = calloc(1, IONIC_PROFILE_FILE_MAX + 1);
        if (file)
            len = fread(file, 1, IONIC_PROFILE_FILE_MAX, f);
        ok = file && len == strlen(text) && memcmp(file, text, len) == 0;
    }
    if (f)
        fclose(f);
    free(file);
    snprintf(desc, sizeof(desc), "built-in %s vs profiles/%s.ini", name, name);
    report(ok, "corner", desc,
           "byte-identical, so the shipped file can't drift");
}

/* Every built-in parses onto a profile far from compat; compat restores it. */
static void check_compat_sets_every_key(void)
{
    struct ionic_profile p;
    char err[256];
    const char *text = ionic_profile_builtin("compat");
    const char *far = "[identity]\nasic_type = salina\nasic_rev = 9\n"
                      "fw_version = far\n[link]\nspeed_mbps = 1000\n"
                      "xcvr = none\nmtu_max = 1500\n[queues]\neth = 64\n"
                      "rdma_eqs = 4\nintr_vectors = auto\n[stats]\n"
                      "interval_ms = 10\n";
    const struct want compat = {COMPAT_VALUES};
    bool ok;

    ionic_profile_defaults(&p);
    ok = ionic_profile_apply_text(&p, "far", far, strlen(far), err,
                                  sizeof(err)) == 0 &&
         text &&
         ionic_profile_apply_text(&p, "compat", text, strlen(text), err,
                                  sizeof(err)) == 0 &&
         profile_is(&p, &compat);
    report(ok, "positive",
           "built-in compat layered on a profile that differs in every key",
           "every key back to compat: the built-in sets each one");
}

/* --print-profile output parses back to the same profile and hash. */
static void check_round_trip(void)
{
    const char *layer = "[identity]\nasic_type = elba\nasic_rev = 3\n"
                        "fw_version = 1.8.0 (E-32)\n[link]\n"
                        "speed_mbps = 400000\nxcvr = fiber\nmtu_max = 9000\n"
                        "[queues]\neth = 9\nrdma_eqs = 5\n"
                        "intr_vectors = auto\n[stats]\ninterval_ms = 250\n";
    struct ionic_profile p, q;
    char err[256], text[2048];
    int n;
    bool ok;

    ionic_profile_defaults(&p);
    ok = ionic_profile_apply_text(&p, "x", layer, strlen(layer), err,
                                  sizeof(err)) == 0;
    n = ionic_profile_format(&p, text, sizeof(text));
    ionic_profile_defaults(&q);
    ok = ok && n > 0 && (size_t)n < sizeof(text) &&
         ionic_profile_apply_text(&q, "printed", text, (size_t)n, err,
                                  sizeof(err)) == 0 &&
         memcmp(&p.identity, &q.identity, sizeof(p.identity)) == 0 &&
         p.link.speed_mbps == q.link.speed_mbps && p.link.xcvr == q.link.xcvr &&
         p.link.mtu_max == q.link.mtu_max &&
         p.stats.interval_ms == q.stats.interval_ms &&
         memcmp(&p.queues, &q.queues, sizeof(p.queues)) == 0 &&
         ionic_profile_hash(&p) == ionic_profile_hash(&q);
    report(ok, "corner", "the printed profile parsed back onto compat",
           "the same values and the same hash (round trip)");
}

/* The hash depends on the values only, not on how they were layered. */
static void check_hash(void)
{
    struct ionic_profile a, b, c;
    char err[256];
    const char *l1 = "[link]\nspeed_mbps = 25000\n";
    const char *l2 = "[link]\nxcvr = fiber\n";
    const char *both = "[link]\nxcvr = fiber\nspeed_mbps = 25000\n";

    ionic_profile_defaults(&a);
    ionic_profile_defaults(&b);
    ionic_profile_defaults(&c);
    (void)ionic_profile_apply_text(&a, "a", l1, strlen(l1), err, sizeof(err));
    (void)ionic_profile_apply_text(&a, "a", l2, strlen(l2), err, sizeof(err));
    (void)ionic_profile_apply_text(&b, "b", both, strlen(both), err,
                                   sizeof(err));
    report(ionic_profile_hash(&a) == ionic_profile_hash(&b), "corner",
           "the same values reached in two layers or in one", "the same hash");
    report(ionic_profile_hash(&a) != ionic_profile_hash(&c), "corner",
           "compat and a profile that differs from it", "different hashes");
}

/* What each built-in profile sets in [queues], layered on compat. */
static void check_builtin_queues(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        const char *name;
        uint32_t eth, rdma_eqs, intr_vectors, resolved;
    } rows[] = {
        {"positive", "compat [queues]", "4 queues, 32 EQs, 32 vectors",
         "compat", 4, 32, 32, 32},
        {"positive", "small-vm [queues]", "2 queues, 4 EQs, auto = 7 vectors",
         "small-vm", 2, 4, 0, 7},
        {"positive", "generic-100g [queues]",
         "16 queues, 32 EQs, auto = 49 vectors", "generic-100g", 16, 32, 0, 49},
        {"positive", "generic-400g [queues]",
         "64 queues, 32 EQs, auto = 97 vectors", "generic-400g", 64, 32, 0, 97},
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        struct ionic_profile p;
        char err[256] = "";
        bool ok;

        ionic_profile_defaults(&p);
        ok = ionic_profile_apply(&p, rows[i].name, err, sizeof(err)) == 0 &&
             p.queues.eth == rows[i].eth &&
             p.queues.rdma_eqs == rows[i].rdma_eqs &&
             p.queues.intr_vectors == rows[i].intr_vectors &&
             ionic_profile_intr_vectors(&p) == rows[i].resolved;
        report(ok, rows[i].category, rows[i].description, rows[i].expected);
        if (!ok)
            printf("     error '%s'; got eth %" PRIu32 " rdma_eqs %" PRIu32
                   " intr_vectors %" PRIu32 " (%" PRIu32 ")\n",
                   err, p.queues.eth, p.queues.rdma_eqs, p.queues.intr_vectors,
                   ionic_profile_intr_vectors(&p));
    }
}

/* ionic_profile_intr_vectors(): auto is 1 + eth + rdma_eqs; a number is kept.
 */
static void check_intr_vectors(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        uint32_t eth, rdma_eqs, intr_vectors, want;
    } rows[] = {
        {"positive", "auto with 16 queues and 32 EQs", "49", 16, 32, 0, 49},
        {"positive", "an explicit 50", "50, whatever the queues", 16, 32, 50,
         50},
        {"boundary", "auto with the fewest queues and EQs", "1 + 1 + 4 = 6", 1,
         4, 0, 6},
        {"boundary", "auto with the most queues and EQs",
         "1 + 64 + 32 = 97, inside the 128-vector table", 64, 32, 0, 97},
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        struct ionic_profile p;
        uint32_t got;

        ionic_profile_defaults(&p);
        p.queues.eth = rows[i].eth;
        p.queues.rdma_eqs = rows[i].rdma_eqs;
        p.queues.intr_vectors = rows[i].intr_vectors;
        got = ionic_profile_intr_vectors(&p);
        report(got == rows[i].want, rows[i].category, rows[i].description,
               rows[i].expected);
        if (got != rows[i].want)
            printf("     got %" PRIu32 "\n", got);
    }
}

/* Too few vectors for every queue and EQ is allowed, but never silent. */
static void check_warnings(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        const char *layer;
        const char *want; /* substring of the warning, or NULL for none */
    } rows[] = {
        {"positive", "compat: 32 vectors for 1 + 4 + 32",
         "warns: today's device already halves EQs on a big guest", "",
         "queues.intr_vectors = 32 is less than 1 + queues.eth + "
         "queues.rdma_eqs = 37"},
        {"positive", "intr_vectors = auto", "no warning",
         "[queues]\nintr_vectors = auto\n", NULL},
        {"boundary", "intr_vectors exactly 1 + eth + rdma_eqs", "no warning",
         "[queues]\neth = 2\nrdma_eqs = 4\nintr_vectors = 7\n", NULL},
        {"boundary", "intr_vectors one short of 1 + eth + rdma_eqs",
         "warns, naming both numbers",
         "[queues]\neth = 2\nrdma_eqs = 4\nintr_vectors = 6\n",
         "= 6 is less than 1 + queues.eth + queues.rdma_eqs = 7"},
        {"boundary", "intr_vectors = 2, the minimum",
         "accepted, with the warning", "[queues]\nintr_vectors = 2\n",
         "halves its RDMA EQs or Ethernet queues to fit"},
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        struct ionic_profile p;
        char err[256] = "", msg[512] = "";
        int warned;
        bool ok;

        ionic_profile_defaults(&p);
        ok = ionic_profile_apply_text(&p, "layer1", rows[i].layer,
                                      strlen(rows[i].layer), err,
                                      sizeof(err)) == 0;
        warned = ionic_profile_warning(&p, msg, sizeof(msg));
        ok = ok && (rows[i].want ? warned == 1 && strstr(msg, rows[i].want)
                                 : warned == 0);
        report(ok, rows[i].category, rows[i].description, rows[i].expected);
        if (!ok)
            printf("     error '%s'; warning %d '%s'\n", err, warned, msg);
    }
}

/* A chain that would overflow is refused, and the profile is unchanged. */
static void check_chain_limit(void)
{
    struct ionic_profile p;
    char err[256] = "";
    int layers = 0;
    bool ok;

    ionic_profile_defaults(&p);
    while (layers < 1000 &&
           ionic_profile_apply(&p, "small-vm", err, sizeof(err)) == 0)
        layers++;
    ok = layers > 10 && layers < 1000 &&
         strstr(err, "too many profile layers") &&
         strlen(p.chain) < IONIC_PROFILE_CHAIN_MAX;
    report(ok, "boundary", "small-vm layered until the name chain is full",
           "the next layer is refused; the chain stays terminated");
}

/* A NUL inside the text is an error naming its line, not an early end. */
static void check_nul_byte(void)
{
    static const char text[] = "[link]\nxcvr = fib\0er\n";
    struct ionic_profile p;
    char err[256] = "";
    const struct want compat = {COMPAT_VALUES};
    bool ok;

    ionic_profile_defaults(&p);
    ok = ionic_profile_apply_text(&p, "layer1", text, sizeof(text) - 1, err,
                                  sizeof(err)) != 0 &&
         strstr(err, "layer1:2: NUL byte in the text") &&
         profile_is(&p, &compat);
    report(ok, "negative", "a NUL byte inside the text",
           "error naming the line; nothing changes");
}

/* Exact file boundaries and a FIFO with no writer exercise the real loader. */
static void check_file_boundaries(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        size_t size;
        bool fifo;
        bool accepted;
    } cases[] = {
        {"positive", "one-byte comment file", "accepted; only chain changes", 1,
         false, true},
        {"boundary", "one byte below 1 MiB", "accepted; only chain changes",
         IONIC_PROFILE_FILE_MAX - 1, false, true},
        {"boundary", "exactly 1 MiB", "accepted; only chain changes",
         IONIC_PROFILE_FILE_MAX, false, true},
        {"boundary", "one byte above 1 MiB",
         "size error; all profile bytes unchanged", IONIC_PROFILE_FILE_MAX + 1,
         false, false},
        {"negative", "FIFO with no writer",
         "prompt file-type error; all profile bytes unchanged", 0, true, false},
    };
    char path[512];

    snprintf(path, sizeof(path), "%s/boundary.ini", g_tmpdir);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct ionic_profile p;
        unsigned char before[sizeof(p)];
        char err[512] = "";
        bool ready = false, ok = false;

        if (cases[i].fifo) {
            ready = mkfifo(path, 0600) == 0;
        } else {
            char *data = malloc(cases[i].size);
            FILE *f = scratch_open(path);

            if (data && f) {
                memset(data, 'x', cases[i].size);
                data[0] = '#';
                ready = fwrite(data, 1, cases[i].size, f) == cases[i].size;
            }
            if (f && fclose(f) != 0)
                ready = false;
            free(data);
        }
        ionic_profile_defaults(&p);
        memcpy(before, &p, sizeof(p));
        if (ready) {
            int rc;

            /* A regression to blocking open fails the test, never hangs CI. */
            alarm(5);
            rc = ionic_profile_apply(&p, path, err, sizeof(err));
            alarm(0);
            if (cases[i].accepted) {
                ok = rc == 0 && strstr(p.chain, path) != NULL;
                memcpy(p.chain, before + offsetof(struct ionic_profile, chain),
                       sizeof(p.chain));
                ok = ok && memcmp(before, &p, sizeof(p)) == 0;
            } else {
                ok = rc != 0 && memcmp(before, &p, sizeof(p)) == 0 &&
                     strstr(err, cases[i].fifo ? "not a regular file"
                                               : "larger than 1048576 bytes");
            }
        }
        report(ok, cases[i].category, cases[i].description, cases[i].expected);
        unlink(path);
    }
}

int main(void)
{
    if (!mkdtemp(g_tmpdir)) {
        printf("FAIL setup: mkdtemp %s\n", g_tmpdir);
        return 1;
    }

    for (size_t i = 0; i < sizeof(text_cases) / sizeof(text_cases[0]); i++)
        run_text_case(&text_cases[i]);
    for (size_t i = 0; i < sizeof(spec_cases) / sizeof(spec_cases[0]); i++)
        run_spec_case(&spec_cases[i]);
    check_builtin_matches_file("compat");
    check_builtin_matches_file("small-vm");
    check_builtin_matches_file("generic-100g");
    check_builtin_matches_file("generic-400g");
    check_builtin_queues();
    check_intr_vectors();
    check_warnings();
    check_compat_sets_every_key();
    check_round_trip();
    check_hash();
    check_chain_limit();
    check_nul_byte();
    check_file_boundaries();

    {
        char path[512];

        snprintf(path, sizeof(path), "%s/fast.ini", g_tmpdir);
        unlink(path);
        snprintf(path, sizeof(path), "%s/bad.ini", g_tmpdir);
        unlink(path);
        snprintf(path, sizeof(path), "%s/big.ini", g_tmpdir);
        unlink(path);
        rmdir(g_tmpdir);
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
