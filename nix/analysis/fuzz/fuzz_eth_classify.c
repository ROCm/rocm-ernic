/*
 * libFuzzer harness for the per-frame classification the emulated ionic NIC
 * does on every Ethernet frame: the destination MAC class the lif_stats
 * counters use (ionic_emu_mac_class()) and the flow hash that steers a
 * received frame to an Rx queue (ionic_eth_flow_hash()).
 *
 * Both read a frame straight off the TAP or the TCP mesh, so every byte is
 * untrusted.  The input is copied to an exact-size heap buffer, so ASan
 * flags any read past it.  On top of that the harness checks:
 *
 *   - the MAC class agrees with a reference: -1 below 6 bytes, broadcast
 *     for all-ones, multicast for the group bit, else unicast;
 *   - the hash is the same on a second call, and the FNV-1a basis below
 *     the 12 bytes of the two MACs;
 *   - inserting an 802.1Q tag after the MACs of a frame that carries fewer
 *     than two tags leaves the hash unchanged, as up to two are skipped;
 *   - hash % n picks a queue below n for every n the profile allows.
 *
 * LLVMFuzzerInitialize() first runs a table of seed frames -- positive,
 * negative, boundary and corner -- through the same checks plus the kind
 * of hash each should produce (L3 or L2), so the oracle itself is tested
 * on every run; a failing seed aborts before any fuzzing starts.  With
 * FUZZ_EMIT_SEEDS=1 it also writes them into the current directory as a seed
 * corpus.
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

#include "ionic_emu_stats.h"
#include "ionic_eth_steer.h"
#include "ionic_profile.h"

#include "fuzz_harness.h"

#define MACS_LEN 12u
#define TAG_LEN  4u

static uint32_t l2_hash(const uint8_t *frame)
{
    uint32_t h = IONIC_ETH_STEER_HASH_EMPTY;

    for (size_t i = 0; i < MACS_LEN; i++) {
        h ^= frame[i];
        h *= 0x01000193u;
    }
    return h;
}

static int ref_mac_class(const uint8_t *frame, size_t len)
{
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    if (len < 6)
        return -1;
    if (memcmp(frame, bcast, 6) == 0)
        return IONIC_EMU_BCAST;
    return (frame[0] & 1u) ? IONIC_EMU_MCAST : IONIC_EMU_UCAST;
}

static bool is_tag(const uint8_t *p)
{
    uint16_t t = (uint16_t)((uint16_t)(p[0] << 8) | p[1]);

    return t == 0x8100u || t == 0x88a8u;
}

/* VLAN tags the hash skips at the front of @frame, up to two. */
static unsigned int leading_tags(const uint8_t *frame, size_t len)
{
    unsigned int n = 0;
    size_t off = MACS_LEN;

    while (n < 2 && len - off >= TAG_LEN && is_tag(frame + off)) {
        n++;
        off += TAG_LEN;
    }
    return n;
}

/* An exact-size heap copy of @len bytes, or NULL. */
static uint8_t *exact_copy(const uint8_t *data, size_t len)
{
    uint8_t *p = malloc(len ? len : 1);

    if (p && len)
        memcpy(p, data, len);
    return p;
}

/*
 * Run every check on @data.  Returns NULL if they all hold, else what
 * failed; *@hash gets the frame's hash.
 */
static const char *check_frame(const uint8_t *data, size_t size, uint32_t *hash)
{
    uint8_t *frame = exact_copy(data, size);
    const char *why = NULL;
    uint32_t h;

    *hash = 0;
    if (!frame)
        return NULL;

    if (ionic_emu_mac_class(frame, size) != ref_mac_class(frame, size)) {
        why = "MAC class differs from the reference";
        goto out;
    }

    h = ionic_eth_flow_hash(frame, size);
    *hash = h;
    if (ionic_eth_flow_hash(frame, size) != h) {
        why = "hash differs on a second call";
        goto out;
    }
    if (size < MACS_LEN && h != IONIC_ETH_STEER_HASH_EMPTY) {
        why = "a frame shorter than the MACs did not hash as empty";
        goto out;
    }
    for (uint32_t n = IONIC_PROFILE_ETH_QUEUES_MIN;
         n <= IONIC_PROFILE_ETH_QUEUES_MAX; n++) {
        if (h % n >= n) {
            why = "steered queue out of range";
            goto out;
        }
    }

    if (size >= MACS_LEN && leading_tags(frame, size) < 2) {
        size_t tlen = size + TAG_LEN;
        uint8_t *tagged = malloc(tlen);

        if (!tagged)
            goto out;
        memcpy(tagged, frame, MACS_LEN);
        tagged[MACS_LEN] = 0x81;
        tagged[MACS_LEN + 1] = 0x00;
        tagged[MACS_LEN + 2] = 0x00;
        tagged[MACS_LEN + 3] = 0x64; /* VID 100 */
        memcpy(tagged + MACS_LEN + TAG_LEN, frame + MACS_LEN, size - MACS_LEN);
        if (ionic_eth_flow_hash(tagged, tlen) != h)
            why = "an added VLAN tag changed the hash";
        free(tagged);
    }

out:
    free(frame);
    return why;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint32_t h;
    const char *why = check_frame(data, size, &h);

    if (why) {
        fprintf(stderr, "fuzz_eth_classify: %s\n", why);
        abort();
    }
    return 0;
}

/* ---- Seeds ----------------------------------------------------------------
 */

#define MACS 0x02, 0, 0, 0, 0, 0x01, 0x02, 0, 0, 0, 0, 0x02
#define IPV4_TCP                                                          \
    0x45, 0, 0, 40, 0, 0, 0x40, 0, 64, 6, 0, 0, 10, 0, 0, 1, 10, 0, 0, 2, \
        0x9c, 0x40, 0x14, 0x51
#define IPV6_UDP                                                              \
    0x60, 0, 0, 0, 0, 8, 17, 64, 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, \
        0, 0, 0, 0, 1, 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  \
        0, 2, 0xc3, 0x50, 0x12, 0xb7

enum kind { KIND_L3, KIND_L2 };

struct seed {
    const char *category;
    const char *description;
    const char *expected;
    const uint8_t *frame;
    size_t len;
    enum kind kind;
};

static const uint8_t s_v4[] = {MACS, 0x08, 0x00, IPV4_TCP};
static const uint8_t s_v6[] = {MACS, 0x86, 0xdd, IPV6_UDP};
static const uint8_t s_vlan[] = {MACS, 0x81, 0x00, 0x00,
                                 0x64, 0x08, 0x00, IPV4_TCP};
static const uint8_t s_qinq[] = {MACS, 0x88, 0xa8, 0x00, 0x0a, 0x81,
                                 0x00, 0x00, 0x64, 0x86, 0xdd, IPV6_UDP};
static const uint8_t s_arp[] = {MACS, 0x08, 0x06, 0, 1, 8, 0, 6, 4, 0, 1};
static const uint8_t s_bcast[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02,
                                  0,    0,    0,    0,    0x01, 0x08, 0x06};
static const uint8_t s_frag[] = {MACS, 0x08, 0x00, 0x45, 0, 0, 40, 0,
                                 0,    0x20, 0,    64,   6, 0, 0,  10,
                                 0,    0,    1,    10,   0, 0, 2};
static const uint8_t s_trunc[] = {MACS, 0x08, 0x00, 0x45, 0,  0, 40,
                                  0,    0,    0x40, 0,    64, 6};
static const uint8_t s_ihl4[] = {MACS, 0x08, 0x00, 0x44, 0, 0, 40, 0,
                                 0,    0x40, 0,    64,   6, 0, 0,  10,
                                 0,    0,    1,    10,   0, 0, 2};
static const uint8_t s_short[] = {0x01, 0x00, 0x5e, 0, 0, 1, 0x02, 0, 0, 0, 0};

static const struct seed seeds[] = {
    {"positive", "IPv4 TCP", "an L3 hash", s_v4, sizeof(s_v4), KIND_L3},
    {"positive", "IPv6 UDP", "an L3 hash", s_v6, sizeof(s_v6), KIND_L3},
    {"positive", "802.1Q IPv4 TCP", "an L3 hash", s_vlan, sizeof(s_vlan),
     KIND_L3},
    {"positive", "QinQ IPv6 UDP", "an L3 hash", s_qinq, sizeof(s_qinq),
     KIND_L3},
    {"corner", "ARP", "the L2 hash", s_arp, sizeof(s_arp), KIND_L2},
    {"corner", "broadcast ARP header", "the L2 hash; class broadcast", s_bcast,
     sizeof(s_bcast), KIND_L2},
    {"corner", "IPv4 first fragment", "an L3 hash without ports", s_frag,
     sizeof(s_frag), KIND_L3},
    {"corner", "IPv4 header cut at 10 bytes", "the L2 hash", s_trunc,
     sizeof(s_trunc), KIND_L2},
    {"negative", "IPv4 ihl 4", "the L2 hash", s_ihl4, sizeof(s_ihl4), KIND_L2},
    {"boundary", "12 bytes: the MACs alone", "the L2 hash", s_v4, MACS_LEN,
     KIND_L2},
    {"boundary", "11 bytes, multicast destination", "the empty hash", s_short,
     sizeof(s_short), KIND_L2},
    {"boundary", "0 bytes", "the empty hash", s_v4, 0, KIND_L2},
};

/* Seed names are generated locally; environment data never becomes a path. */
static void emit_seed(size_t i, const uint8_t *buf, size_t len)
{
    char path[32];
    int fd;
    FILE *f;
    bool ok;

    snprintf(path, sizeof(path), "seed-%02zu", i);
    /* Never overwrite an existing file or follow a planted symlink. */
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "fuzz_eth_classify: cannot create %s\n", path);
        exit(EXIT_FAILURE);
    }
    f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        fprintf(stderr, "fuzz_eth_classify: cannot open %s as a stream\n",
                path);
        exit(EXIT_FAILURE);
    }
    ok = fwrite(buf, 1, len, f) == len;
    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        fprintf(stderr, "fuzz_eth_classify: cannot write %s\n", path);
        exit(EXIT_FAILURE);
    }
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    const char *emit = getenv("FUZZ_EMIT_SEEDS");
    bool emit_seeds = emit && strcmp(emit, "1") == 0;
    int failures = 0;

    (void)argc;
    (void)argv;
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        const struct seed *s = &seeds[i];
        uint32_t h;
        const char *why = check_frame(s->frame, s->len, &h);
        bool l2 = s->len >= MACS_LEN ? h == l2_hash(s->frame)
                                     : h == IONIC_ETH_STEER_HASH_EMPTY;
        bool ok = why == NULL && l2 == (s->kind == KIND_L2);

        printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", s->category,
               s->description, s->expected);
        if (!ok) {
            if (why)
                printf("     %s\n", why);
            failures++;
        }
        if (emit_seeds)
            emit_seed(i, s->frame, s->len);
    }
    if (failures) {
        fprintf(stderr, "fuzz_eth_classify: %d seed(s) failed\n", failures);
        exit(1);
    }
    return 0;
}
