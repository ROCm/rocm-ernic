/*
 * Unit tests for the ionic emulator's queue and interrupt shape.
 *
 * The guest driver sizes itself from three identity numbers: the LIF's
 * Rx/Tx queue counts, its RDMA EQ count and the device's interrupt count.
 * ionic_lif_size() then wants 1 + min(queues, vCPUs) + min(EQs, vCPUs)
 * vectors and halves its EQs or queues until that fits.  These rows check
 * that each NIC profile's [queues] keys reach the identity, that the
 * vector budget fits where the profile says it should, and that queue
 * indices past the profile's count are refused without touching memory.
 * That every one of the 128 vectors has room in BAR0 is a compile-time
 * check, in rocm_ernic_internal.h and ionic_eth_emu_priv.h.
 *
 * Then Rx steering: the flow hash against an independent FNV-1a reference
 * over well-formed, tagged, fragmented and truncated frames; the set of Rx
 * queues Q_INIT and Q_CONTROL leave active; and frames from the TAP and
 * the inbox landing on, and interrupting, the queue their flow steers to.
 *
 * The test links the eth emulator and calls its devcmd handlers through
 * ionic_eth_emu_priv.h, and the DMA entry points are backed by a plain
 * buffer (ionic_eth_emu_fakes.h).
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "byteorder.h"
#include "ionic_emu_stats.h"
#include "ionic_eth_emu.h"
#include "ionic_eth_emu_fakes.h"
#include "ionic_eth_emu_priv.h"
#include "ionic_eth_steer.h"
#include "ionic_fault.h"
#include "ionic_profile.h"
#include "ionic_status.h"
#include "rocm_ernic_internal.h"

/* Offsets in union ionic_dev_identity and union ionic_lif_identity
 * (ionic_if.h), written here independently of the emulator's copies. */
#define DEVID_NINTRS_OFF        0x0cu
#define DEVID_EQ_COUNT_OFF      0x1cu
#define LIFID_CONFIG_QCOUNT_OFF 0x0b0u /* le32 queue_count[16], by qtype */
#define QTYPE_SZ                12u    /* struct ionic_lif_logical_qtype */

static int failures;

static void report(bool ok, const char *category, const char *description,
                   const char *expected)
{
    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", category,
           description, expected);
    if (!ok)
        failures++;
}

/* ---- Helpers ------------------------------------------------------------ */

/*
 * A fresh device shaped by built-in profile @name (NULL: compat) and then
 * ini @layer (NULL: none).  Aborts if either is refused: rows only use
 * valid profiles.  abort() rather than exit(), which isn't thread safe.
 */
static struct ionic_eth_emu *new_emu(const char *name, const char *layer)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);
    struct ionic_profile p;
    char err[256] = "";

    if (!emu) {
        fprintf(stderr, "ionic_eth_emu_create failed\n");
        abort();
    }
    ionic_profile_defaults(&p);
    if ((name && ionic_profile_apply(&p, name, err, sizeof(err)) != 0) ||
        (layer && ionic_profile_apply_text(&p, "row", layer, strlen(layer), err,
                                           sizeof(err)) != 0)) {
        fprintf(stderr, "profile refused: %s\n", err);
        abort();
    }
    ionic_eth_emu_set_profile(emu, &p);
    memset(g_mem, 0, sizeof(g_mem));
    g_nwrites = 0;
    return emu;
}

/* What the identity blocks tell the driver. */
struct shape {
    uint32_t rxq, txq;   /* LIF identity queue_count[RXQ], [TXQ] */
    uint32_t lif_eqs;    /* LIF identity rdma eq_qtype qid_count */
    uint32_t dev_eqs;    /* dev identity eq_count */
    uint32_t dev_nintrs; /* dev identity nintrs */
};

static void read_shape(struct ionic_eth_emu *emu, struct shape *s)
{
    static uint8_t data[DEVCMD_DATA_SIZE];
    const uint8_t cmd[64] = {0};
    uint8_t comp[16] = {0};

    memset(data, 0, sizeof(data));
    ionic_eth_emu_handle_identify(emu, cmd, comp, data);
    s->dev_nintrs = ernic_get_le32(data + DEVID_NINTRS_OFF);
    s->dev_eqs = ernic_get_le32(data + DEVID_EQ_COUNT_OFF);

    memset(data, 0, sizeof(data));
    ionic_eth_emu_handle_lif_identify(emu, cmd, comp, data);
    s->rxq =
        ernic_get_le32(data + LIFID_CONFIG_QCOUNT_OFF + 4u * IONIC_QTYPE_RXQ);
    s->txq =
        ernic_get_le32(data + LIFID_CONFIG_QCOUNT_OFF + 4u * IONIC_QTYPE_TXQ);
    /* eq_qtype is the fifth ionic_lif_logical_qtype after rdma+32. */
    s->lif_eqs =
        ernic_get_le32(data + LIF_ID_RDMA_OFF + 32u + 4u * QTYPE_SZ + 4u);
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

/*
 * The vectors ionic_lif_size() (ionic_lif.c) asks for on its first try:
 * one for the adminq and notifyq, one per queue pair and one per EQ, each
 * capped at the online vCPUs.  If that exceeds nintrs it halves.
 */
static uint32_t driver_nintrs(const struct shape *s, uint32_t cpus)
{
    uint32_t nxqs = min_u32(min_u32(s->txq, s->rxq), cpus);

    return 1u + nxqs + min_u32(s->lif_eqs, cpus);
}

/* Q_INIT for @qtype/@index with a 4-entry ring at @ring. */
static void q_init(struct ionic_eth_emu *emu, uint8_t qtype, uint32_t index,
                   uint16_t intr, uint64_t ring, uint64_t cq)
{
    uint8_t cmd[64] = {0};
    uint8_t comp[16] = {0};

    cmd[0] = IONIC_CMD_Q_INIT;
    cmd[4] = qtype;
    ernic_put_le32(cmd + 8, index);
    ernic_put_le16(cmd + 14, intr);
    cmd[19] = 2; /* log2 of the ring depth */
    ernic_put_le64(cmd + 20, ring);
    ernic_put_le64(cmd + 28, cq);
    ionic_eth_emu_handle_q_init(emu, cmd, comp);
}

/* ---- Identity and the vector budget ------------------------------------ */

struct shape_case {
    const char *category;
    const char *description;
    const char *expected;
    const char *profile; /* built-in name, or NULL for compat */
    const char *layer;   /* ini text on top, or NULL */
    uint32_t cpus;       /* the guest's online vCPUs */
    struct shape want;
    bool want_fits; /* the driver's first try fits nintrs */
};

static const struct shape_case shape_cases[] = {
    {"positive",
     "generic-400g on a 64-vCPU guest",
     "64 queues, 32 EQs, 97 vectors; 1 + 64 + 32 = 97 fits, no halving",
     "generic-400g",
     NULL,
     64,
     {64, 64, 32, 32, 97},
     true},
    {"positive",
     "generic-100g on an 8-vCPU guest",
     "16 queues, 49 vectors; the driver wants 1 + 8 + 8 = 17, which fits",
     "generic-100g",
     NULL,
     8,
     {16, 16, 32, 32, 49},
     true},
    {"positive",
     "queues.eth = 16 with intr_vectors = auto on 16 vCPUs",
     "16 queues, 49 vectors; 1 + 16 + 16 = 33 fits",
     NULL,
     "[queues]\neth = 16\nintr_vectors = auto\n",
     16,
     {16, 16, 32, 32, 49},
     true},
    {"positive",
     "small-vm on a 2-vCPU guest",
     "2 queues, 4 EQs, 7 vectors; 1 + 2 + 2 = 5 fits",
     "small-vm",
     NULL,
     2,
     {2, 2, 4, 4, 7},
     true},
    {"boundary",
     "queues.eth = 1 on a 1-vCPU guest",
     "1 queue; auto gives 1 + 1 + 32 = 34 vectors and the driver wants 3",
     NULL,
     "[queues]\neth = 1\nintr_vectors = auto\n",
     1,
     {1, 1, 32, 32, 34},
     true},
    {"boundary",
     "queues.eth = 64 with intr_vectors = auto on 128 vCPUs",
     "64 queues; 1 + 64 + 32 = 97, the most a profile can need, fits",
     NULL,
     "[queues]\neth = 64\nintr_vectors = auto\n",
     128,
     {64, 64, 32, 32, 97},
     true},
    {"boundary",
     "queues.intr_vectors = 2 on a 4-vCPU guest",
     "nintrs 2; the driver wants 9, so it halves",
     NULL,
     "[queues]\nintr_vectors = 2\n",
     4,
     {4, 4, 32, 32, 2},
     false},
    {"boundary",
     "queues.rdma_eqs = 4",
     "4 EQs in both the dev and the LIF identity",
     NULL,
     "[queues]\nrdma_eqs = 4\n",
     4,
     {4, 4, 4, 4, 32},
     true},
    {"corner",
     "compat on a 64-vCPU guest",
     "4 queues, 32 EQs, 32 vectors as before; 1 + 4 + 32 = 37 > 32, so the "
     "driver halves its EQs, as it always has",
     NULL,
     NULL,
     64,
     {4, 4, 32, 32, 32},
     false},
    {"corner",
     "queues.eth = 64 with compat's 32 vectors on 64 vCPUs",
     "allowed: the driver halves to fit (the server warns)",
     NULL,
     "[queues]\neth = 64\n",
     64,
     {64, 64, 32, 32, 32},
     false},
};

static void run_shape_case(const struct shape_case *c)
{
    struct ionic_eth_emu *emu = new_emu(c->profile, c->layer);
    struct shape got;
    bool fits, ok;

    read_shape(emu, &got);
    fits = driver_nintrs(&got, c->cpus) <= got.dev_nintrs;
    ok = memcmp(&got, &c->want, sizeof(got)) == 0 && fits == c->want_fits &&
         got.dev_nintrs <= IONIC_MSIX_MAX_VECTORS;
    report(ok, c->category, c->description, c->expected);
    if (!ok)
        printf("     got rxq %" PRIu32 " txq %" PRIu32 " lif_eqs %" PRIu32
               " dev_eqs %" PRIu32 " nintrs %" PRIu32 "; driver wants %" PRIu32
               " on %" PRIu32 " vCPUs\n",
               got.rxq, got.txq, got.lif_eqs, got.dev_eqs, got.dev_nintrs,
               driver_nintrs(&got, c->cpus), c->cpus);
    ionic_eth_emu_destroy(emu);
}

/* ---- Queue indices past the profile's count ---------------------------- */

#define RING 0x0100u
#define CQ   0x0200u

struct qinit_case {
    const char *category;
    const char *description;
    const char *expected;
    const char *layer;
    uint8_t qtype;
    uint32_t index;
    bool want_valid;
};

static const struct qinit_case qinit_cases[] = {
    {"positive", "Rx queue 15 with queues.eth = 16", "initialized",
     "[queues]\neth = 16\n", IONIC_QTYPE_RXQ, 15, true},
    {"boundary", "Rx queue 0 with queues.eth = 1", "initialized",
     "[queues]\neth = 1\n", IONIC_QTYPE_RXQ, 0, true},
    {"boundary", "Rx queue 1 with queues.eth = 1", "refused: past the count",
     "[queues]\neth = 1\n", IONIC_QTYPE_RXQ, 1, false},
    {"boundary", "Tx queue 63 with queues.eth = 64", "initialized",
     "[queues]\neth = 64\n", IONIC_QTYPE_TXQ, 63, true},
    {"boundary", "Tx queue 64 with queues.eth = 64",
     "refused: past the array, with no write out of bounds",
     "[queues]\neth = 64\n", IONIC_QTYPE_TXQ, 64, false},
    {"negative", "Rx queue 4 with compat's 4 queues", "refused", NULL,
     IONIC_QTYPE_RXQ, 4, false},
    {"negative", "Rx queue 0xffffffff", "refused", "[queues]\neth = 64\n",
     IONIC_QTYPE_RXQ, 0xffffffffu, false},
};

static void run_qinit_case(const struct qinit_case *c)
{
    struct ionic_eth_emu *emu = new_emu(NULL, c->layer);
    bool valid = false;

    q_init(emu, c->qtype, c->index, 1, RING, CQ);
    for (uint32_t i = 0; i < IONIC_EMU_ETH_QMAX; i++)
        if (emu->eth_q[c->qtype][i].valid)
            valid = valid || i == c->index;
    report(valid == c->want_valid, c->category, c->description, c->expected);
    ionic_eth_emu_destroy(emu);
}

/* A Tx doorbell for a queue past the count moves nothing. */
static void check_txq_past_count(void)
{
    struct ionic_eth_emu *emu = new_emu(NULL, NULL);
    /* p_index 1 in bits 15:0, qid 4 in bits 31:24. */
    uint64_t db = 1u | (UINT64_C(4) << 24);

    q_init(emu, IONIC_QTYPE_TXQ, 3, 1, RING, CQ);
    g_nwrites = 0;
    ionic_eth_emu_bar2_access(emu, (char *)&db, sizeof(db),
                              (loff_t)IONIC_QTYPE_TXQ * 8, true);
    report(g_nwrites == 0 && !emu->eth_q[IONIC_QTYPE_TXQ][4].valid, "negative",
           "a Tx doorbell for queue 4 of 4", "ignored: no DMA, no state");
    ionic_eth_emu_destroy(emu);
}

/* ---- Vectors ------------------------------------------------------------ */

static void check_vectors(void)
{
    struct ionic_eth_emu *emu = new_emu("generic-400g", NULL);
    uint32_t zero = 0;
    loff_t mask127 = (loff_t)IONIC_BAR0_INTR_CTRL_OFFSET +
                     127 * INTR_REG_STRIDE + INTR_MASK_OFF;

    report(
        ionic_eth_emu_trigger_irq(emu, 127) == 0 && emu->intr_pending[127] == 1,
        "boundary", "vector 127 raised while masked", "accepted and latched");
    report(ionic_eth_emu_trigger_irq(emu, IONIC_MSIX_MAX_VECTORS) == -EINVAL,
           "boundary", "vector 128", "refused with -EINVAL");
    report(ionic_eth_emu_bar0_access(emu, (char *)&zero, 4, mask127, true) ==
                   4 &&
               emu->intr_mask[127] == 0 && emu->intr_pending[127] == 0,
           "corner", "unmask vector 127 through its intr_ctrl block",
           "unmasked, and the latched interrupt delivered");
    ionic_eth_emu_destroy(emu);
}

/* ---- Flow hash ---------------------------------------------------------- */

/* A reference FNV-1a 32, written independently of ionic_eth_steer.c. */
static uint32_t ref_fnv(uint32_t h, const uint8_t *p, size_t n)
{
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

#define REF_BASIS 2166136261u

/* What the reference hashes for a row. */
enum ref_kind {
    REF_PORTS, /* addresses, protocol and ports */
    REF_L3,    /* addresses and protocol */
    REF_L2,    /* the two MACs */
    REF_EMPTY, /* nothing */
};

/* One frame to build: Ethernet, up to three tags, then an IP header. */
struct flow {
    uint16_t tags[3]; /* TPIDs, outermost first; 0 ends the list */
    uint16_t ethertype;
    uint8_t version; /* 0: the EtherType's own */
    uint8_t ihl;     /* IPv4 header words; 0: 5 */
    uint8_t proto;   /* IPv4 protocol or IPv6 next header */
    uint16_t frag;   /* IPv4 flags and fragment offset */
    uint8_t src[16], dst[16];
    uint16_t sport, dport;
};

static const uint8_t frame_macs[12] = {0x02, 0, 0, 0, 0, 0x01,
                                       0x02, 0, 0, 0, 0, 0x02};

static bool flow_is_v6(const struct flow *fl)
{
    return fl->ethertype == 0x86dd;
}

/* Build @fl into @f (at least 128 bytes): MACs, tags, IP, ports, 8 bytes of
 * payload.  Returns the frame length and sets *@l3 and *@l4. */
static size_t build_flow(uint8_t *f, const struct flow *fl, size_t *l3,
                         size_t *l4)
{
    size_t off = 12;

    memset(f, 0xa5, 128);
    memcpy(f, frame_macs, 12);
    for (unsigned int i = 0; i < 3 && fl->tags[i]; i++) {
        ernic_put_be16(f + off, fl->tags[i]);
        /* TCI: VID 256 + i */
        ernic_put_be16(f + off + 2, (uint16_t)(0x0100u + i));
        off += 4;
    }
    ernic_put_be16(f + off, fl->ethertype);
    off += 2;
    *l3 = off;
    if (flow_is_v6(fl)) {
        memset(f + off, 0, 40);
        f[off] = (uint8_t)((fl->version ? fl->version : 6) << 4);
        f[off + 6] = fl->proto;
        f[off + 7] = 64;
        memcpy(f + off + 8, fl->src, 16);
        memcpy(f + off + 24, fl->dst, 16);
        off += 40;
    } else {
        uint8_t ihl = fl->ihl ? fl->ihl : 5;

        memset(f + off, 0, (size_t)ihl * 4u);
        f[off] = (uint8_t)(((fl->version ? fl->version : 4) << 4) | ihl);
        ernic_put_be16(f + off + 6, fl->frag);
        f[off + 8] = 64;
        f[off + 9] = fl->proto;
        memcpy(f + off + 12, fl->src, 4);
        memcpy(f + off + 16, fl->dst, 4);
        off += (size_t)ihl * 4u;
    }
    *l4 = off;
    ernic_put_be16(f + off, fl->sport);
    ernic_put_be16(f + off + 2, fl->dport);
    return off + 4 + 8;
}

/* The reference hash of a frame built from @fl. */
static uint32_t ref_hash(const struct flow *fl, enum ref_kind kind,
                         const uint8_t *f, size_t l4)
{
    size_t alen = flow_is_v6(fl) ? 16 : 4;
    uint32_t h = REF_BASIS;

    if (kind == REF_EMPTY)
        return REF_BASIS;
    if (kind == REF_L2)
        return ref_fnv(REF_BASIS, frame_macs, 12);
    h = ref_fnv(h, fl->src, alen);
    h = ref_fnv(h, fl->dst, alen);
    h = ref_fnv(h, &fl->proto, 1);
    if (kind == REF_PORTS)
        h = ref_fnv(h, f + l4, 4);
    return h;
}

#define V4(a, b, c, d) \
    {                  \
        a, b, c, d     \
    }
#define V6(last)                              \
    {                                         \
        0x20, 0x01, 0x0d, 0xb8, [15] = (last) \
    }

struct hash_case {
    const char *category;
    const char *description;
    const char *expected;
    struct flow fl;
    long len; /* bytes of the built frame to hash: -1 all; -2 up to l4;
               * -3 l3 + 10; else exactly this many */
    enum ref_kind want;
};

/*
 * The EtherType and protocol are arguments, not defaults a row overrides,
 * so no row initializes a field twice.
 */
#define FL_IP4(etype, ipproto, ...)                                        \
    {                                                                      \
        .ethertype = (etype), .proto = (ipproto), .src = V4(10, 0, 0, 1),  \
        .dst = V4(10, 0, 0, 2), .sport = 40000, .dport = 5201, __VA_ARGS__ \
    }
#define FL_IP6(ipproto, ...)                                                 \
    {                                                                        \
        .ethertype = 0x86dd, .proto = (ipproto), .src = V6(1), .dst = V6(2), \
        .sport = 50000, .dport = 4791, __VA_ARGS__                           \
    }
#define FL_TCP4(...) FL_IP4(0x0800, 6, __VA_ARGS__)
#define FL_UDP6(...) FL_IP6(17, __VA_ARGS__)

static const struct hash_case hash_cases[] = {
    {"positive", "IPv4 TCP 5-tuple", "fnv1a(src, dst, proto, ports)", FL_TCP4(),
     -1, REF_PORTS},
    {"positive", "IPv6 UDP 5-tuple", "fnv1a(src, dst, nh, ports), v6 addrs",
     FL_UDP6(), -1, REF_PORTS},
    {"positive", "IPv4 SCTP", "ports hashed", FL_IP4(0x0800, 132, ), -1,
     REF_PORTS},
    {"positive", "802.1Q-tagged IPv4 TCP",
     "the tag skipped: the same hash as untagged", FL_TCP4(.tags = {0x8100}),
     -1, REF_PORTS},
    {"positive", "QinQ (0x88a8 then 0x8100) IPv6 UDP",
     "both tags skipped: the same hash as untagged",
     FL_UDP6(.tags = {0x88a8, 0x8100}), -1, REF_PORTS},
    {"corner", "three VLAN tags", "the third is not skipped: L2 hash",
     FL_TCP4(.tags = {0x88a8, 0x8100, 0x8100}), -1, REF_L2},
    {"corner", "ARP", "L2 hash of the two MACs", FL_IP4(0x0806, 6, ), -1,
     REF_L2},
    {"corner", "IPv4 first fragment (MF set)",
     "no ports, so every fragment of the datagram hashes alike",
     FL_TCP4(.frag = 0x2000), -1, REF_L3},
    {"corner", "IPv4 later fragment (offset 185)", "no ports",
     FL_TCP4(.frag = 185), -1, REF_L3},
    {"boundary", "IPv4 DF set, not a fragment", "ports hashed",
     FL_TCP4(.frag = 0x4000), -1, REF_PORTS},
    {"corner", "IPv4 with options (ihl 6)", "ports read past the options",
     FL_TCP4(.ihl = 6), -1, REF_PORTS},
    {"boundary", "IPv4 ihl 15 (60-byte header)", "ports read at 60",
     FL_TCP4(.ihl = 15), -1, REF_PORTS},
    {"corner", "IPv4 ICMP", "no ports for a portless protocol",
     FL_IP4(0x0800, 1, ), -1, REF_L3},
    {"corner", "IPv4 header truncated (14 + 10 bytes)",
     "L2 hash, no read past the frame", FL_TCP4(), -3, REF_L2},
    {"negative", "IPv4 ihl 4", "L2 hash: header length invalid",
     FL_TCP4(.ihl = 4), -1, REF_L2},
    {"negative", "IPv4 ihl 15 in a 54-byte frame",
     "L2 hash: options run past the frame", FL_TCP4(.ihl = 15), 54, REF_L2},
    {"negative", "version 6 under EtherType 0x0800",
     "L2 hash: version mismatch", FL_TCP4(.version = 6), -1, REF_L2},
    {"negative", "version 4 under EtherType 0x86dd",
     "L2 hash: version mismatch", FL_UDP6(.version = 4), -1, REF_L2},
    {"corner", "IPv6 header one byte short (39)", "L2 hash", FL_UDP6(), 14 + 39,
     REF_L2},
    {"corner", "IPv6 fragment header (nh 44)",
     "no ports: extension headers not walked", FL_IP6(44, ), -1, REF_L3},
    {"boundary", "IPv4 TCP cut at the end of the IP header",
     "addresses and protocol only", FL_TCP4(), -2, REF_L3},
    {"boundary", "IPv4 TCP with 3 of the 4 port bytes", "no ports", FL_TCP4(),
     14 + 20 + 3, REF_L3},
    {"boundary", "IPv4 TCP with exactly the 4 port bytes", "ports hashed",
     FL_TCP4(), 14 + 20 + 4, REF_PORTS},
    {"boundary", "a 13-byte frame", "L2 hash: no EtherType", FL_TCP4(), 13,
     REF_L2},
    {"boundary", "a 12-byte frame", "L2 hash of exactly the MACs", FL_TCP4(),
     12, REF_L2},
    {"boundary", "an 11-byte frame", "the empty hash", FL_TCP4(), 11,
     REF_EMPTY},
    {"boundary", "a 0-byte frame", "the empty hash", FL_TCP4(), 0, REF_EMPTY},
};

static void run_hash_case(const struct hash_case *c)
{
    uint8_t f[128];
    size_t l3, l4;
    size_t full = build_flow(f, &c->fl, &l3, &l4);
    size_t len = c->len == -1   ? full
                 : c->len == -2 ? l4
                 : c->len == -3 ? l3 + 10
                                : (size_t)c->len;
    /* An exact-size heap copy, so ASan flags any read past @len. */
    uint8_t *heap = malloc(len ? len : 1);
    uint32_t want = ref_hash(&c->fl, c->want, f, l4);
    uint32_t got;

    if (!heap) {
        fprintf(stderr, "out of memory\n");
        abort();
    }
    memcpy(heap, f, len);
    got = ionic_eth_flow_hash(heap, len);
    free(heap);
    if (got != want) {
        printf("FAIL %s: %s; expected: %s (0x%08" PRIx32 "); got: 0x%08" PRIx32
               "\n",
               c->category, c->description, c->expected, want, got);
        failures++;
        return;
    }
    report(true, c->category, c->description, c->expected);
}

/* ---- Steering ----------------------------------------------------------- */

/* A deterministic generator, so a failing row always fails the same way. */
static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

/* A random IPv4 UDP or TCP 5-tuple frame in @f; returns its length. */
static size_t random_frame(uint8_t *f, uint32_t *seed)
{
    struct flow fl = FL_TCP4();
    size_t l3, l4;
    uint32_t a = lcg(seed), b = lcg(seed), p = lcg(seed);

    fl.proto = (p & 1u) ? 6 : 17;
    memcpy(fl.src, &a, 4);
    memcpy(fl.dst, &b, 4);
    fl.sport = (uint16_t)(p >> 8);
    fl.dport = (uint16_t)(p >> 20);
    return build_flow(f, &fl, &l3, &l4);
}

static void check_steer_basic(void)
{
    uint8_t act[IONIC_EMU_ETH_QMAX], f[128];
    uint32_t seed = 1;
    size_t len;
    bool same = true, all0 = true;
    uint32_t hits[IONIC_EMU_ETH_QMAX];
    uint8_t first;

    for (uint32_t i = 0; i < IONIC_EMU_ETH_QMAX; i++)
        act[i] = (uint8_t)i;

    len = random_frame(f, &seed);
    first = ionic_eth_emu_rx_steer(act, 16, f, len);
    for (int i = 0; i < 1000; i++)
        same = same && ionic_eth_emu_rx_steer(act, 16, f, len) == first;
    report(same, "corner", "the same 5-tuple steered 1,000 times",
           "the same queue each time");

    for (int i = 0; i < 4096; i++) {
        len = random_frame(f, &seed);
        all0 = all0 && ionic_eth_emu_rx_steer(act, 1, f, len) == 0;
    }
    report(all0, "boundary", "4,096 random tuples, n = 1",
           "every one to queue 0");

    static const uint32_t ns[] = {16, 64};
    for (size_t k = 0; k < 2; k++) {
        uint32_t n = ns[k], missing = 0, lo = UINT32_MAX, hi = 0;
        char desc[96];

        memset(hits, 0, sizeof(hits));
        for (int i = 0; i < 4096; i++) {
            len = random_frame(f, &seed);
            hits[ionic_eth_emu_rx_steer(act, n, f, len)]++;
        }
        for (uint32_t q = 0; q < n; q++) {
            missing += hits[q] == 0;
            lo = hits[q] < lo ? hits[q] : lo;
            hi = hits[q] > hi ? hits[q] : hi;
        }
        snprintf(desc, sizeof(desc),
                 "every queue reachable: 4,096 random tuples, n = %" PRIu32, n);
        report(missing == 0, n == 64 ? "boundary" : "corner", desc,
               "every queue gets at least one frame");
        /* Binomial(4096, 1/n): within 4 sigma, compared as squares. */
        double mean = 4096.0 / n, var = mean * (1.0 - 1.0 / n);
        double dlo = (double)lo - mean, dhi = (double)hi - mean;
        snprintf(desc, sizeof(desc),
                 "spread of 4,096 random tuples over n = %" PRIu32
                 " (min %" PRIu32 ", max %" PRIu32 ")",
                 n, lo, hi);
        report(dlo * dlo <= 16 * var && dhi * dhi <= 16 * var, "statistical",
               desc, "every queue within 4 sigma of 4096 / n");
    }
}

/* ---- Which Rx queues are active ----------------------------------------- */

/* Q_CONTROL as the driver posts it, over the Ethernet adminq. */
static void q_control(struct ionic_eth_emu *emu, uint8_t qtype, uint32_t index,
                      uint8_t oper)
{
    uint8_t cmd[64] = {0}, comp[16] = {0xff};

    cmd[0] = IONIC_CMD_Q_CONTROL;
    cmd[1] = qtype;
    ernic_put_le32(cmd + 4, index);
    cmd[8] = oper;
    ionic_eth_emu_process_adminq_cmd(emu, cmd, comp);
    if (comp[0] != IONIC_RC_SUCCESS) {
        printf("FAIL setup: Q_CONTROL status %u\n", comp[0]);
        failures++;
    }
}

/* ionic_if.h IONIC_Q_HANG_RESET; the device only acknowledges it. */
#define Q_HANG_RESET 2

enum op_kind { OP_END, OP_INIT, OP_CTL };

struct q_op {
    enum op_kind kind;
    uint8_t qtype;
    uint32_t index;
    uint8_t oper;
};

struct active_case {
    const char *category;
    const char *description;
    const char *expected;
    const char *layer;
    uint64_t init;   /* Rx queues given Q_INIT */
    uint64_t enable; /* then Q_CONTROL ENABLE */
    struct q_op ops[3];
    uint64_t want; /* the active set */
};

#define ALL64 UINT64_MAX
#define ETH16 "[queues]\neth = 16\n"
#define ETH64 "[queues]\neth = 64\n"

static const struct active_case active_cases[] = {
    {"positive",
     "16 queues, all enabled",
     "all 16 active and reachable",
     ETH16,
     0xffff,
     0xffff,
     {{.kind = OP_END}},
     0xffff},
    {"corner",
     "the guest enables 8 of 16 queues",
     "steering mod the active count: queues 0-7 only, each reached",
     ETH16,
     0xffff,
     0x00ff,
     {{.kind = OP_END}},
     0x00ff},
    {"corner",
     "queues 0, 2 and 5 enabled",
     "only those, each reached",
     ETH16,
     0xffff,
     0x25,
     {{.kind = OP_END}},
     0x25},
    {"negative",
     "16 initialized, none enabled",
     "none active",
     ETH16,
     0xffff,
     0,
     {{.kind = OP_END}},
     0},
    {"negative",
     "ENABLE before Q_INIT on queues 2 and 3",
     "only the initialized queues 0 and 1 count",
     ETH16,
     0x3,
     0xf,
     {{.kind = OP_END}},
     0x3},
    {"corner",
     "queue 1 disabled after enable",
     "0, 2 and 3 active",
     NULL,
     0xf,
     0xf,
     {{OP_CTL, IONIC_QTYPE_RXQ, 1, IONIC_Q_DISABLE}},
     0xd},
    {"corner",
     "Q_INIT again on queue 0 after enable",
     "queue 0 needs a fresh ENABLE",
     NULL,
     0xf,
     0xf,
     {{OP_INIT, IONIC_QTYPE_RXQ, 0, 0}},
     0xe},
    {"corner",
     "re-init then ENABLE on queue 0",
     "all four active again",
     NULL,
     0xf,
     0xf,
     {{OP_INIT, IONIC_QTYPE_RXQ, 0, 0},
      {OP_CTL, IONIC_QTYPE_RXQ, 0, IONIC_Q_ENABLE}},
     0xf},
    {"corner",
     "HANG_RESET on an enabled queue",
     "no effect",
     NULL,
     0xf,
     0xf,
     {{OP_CTL, IONIC_QTYPE_RXQ, 2, Q_HANG_RESET}},
     0xf},
    {"negative",
     "an unknown oper (7)",
     "no effect",
     NULL,
     0xf,
     0x1,
     {{OP_CTL, IONIC_QTYPE_RXQ, 1, 7}},
     0x1},
    {"negative",
     "ENABLE of Tx queue 1",
     "the Rx set is unchanged",
     NULL,
     0xf,
     0x1,
     {{OP_CTL, IONIC_QTYPE_TXQ, 1, IONIC_Q_ENABLE}},
     0x1},
    {"negative",
     "ENABLE of Rx queue 4 with compat's 4 queues",
     "ignored, with no write past the array",
     NULL,
     0xf,
     0xf,
     {{OP_CTL, IONIC_QTYPE_RXQ, 4, IONIC_Q_ENABLE}},
     0xf},
    {"negative",
     "ENABLE of qtype 9, index 0xffffffff",
     "ignored",
     NULL,
     0xf,
     0xf,
     {{OP_CTL, 9, 0xffffffffu, IONIC_Q_ENABLE}},
     0xf},
    {"boundary",
     "queues.eth = 1, queue 0 enabled",
     "only queue 0",
     "[queues]\neth = 1\n",
     0x1,
     0x1,
     {{.kind = OP_END}},
     0x1},
    {"boundary",
     "queues.eth = 64, all enabled",
     "all 64 active and reachable",
     ETH64,
     ALL64,
     ALL64,
     {{.kind = OP_END}},
     ALL64},
};

static void run_active_case(const struct active_case *c)
{
    struct ionic_eth_emu *emu = new_emu(NULL, c->layer);
    uint8_t act[IONIC_EMU_ETH_QMAX], f[128];
    uint64_t got = 0, reached = 0, stray = 0;
    uint32_t n, seed = 7;

    for (uint32_t i = 0; i < 64; i++)
        if (c->init & (1ull << i))
            q_init(emu, IONIC_QTYPE_RXQ, i, (uint16_t)(1 + i), RING, CQ);
    for (uint32_t i = 0; i < 64; i++)
        if (c->enable & (1ull << i))
            q_control(emu, IONIC_QTYPE_RXQ, i, IONIC_Q_ENABLE);
    for (size_t k = 0; k < 3 && c->ops[k].kind != OP_END; k++) {
        const struct q_op *op = &c->ops[k];

        if (op->kind == OP_INIT)
            q_init(emu, op->qtype, op->index, 1, RING, CQ);
        else
            q_control(emu, op->qtype, op->index, op->oper);
    }

    n = ionic_eth_emu_rx_active(emu, act);
    for (uint32_t i = 0; i < n; i++)
        got |= 1ull << act[i];
    for (int i = 0; n && i < 4096; i++) {
        size_t len = random_frame(f, &seed);
        uint8_t q = ionic_eth_emu_rx_steer(act, n, f, len);

        reached |= 1ull << q;
        stray |= (1ull << q) & ~c->want;
    }
    if (got != c->want || (n && (reached != c->want || stray))) {
        printf("FAIL %s: %s; expected: %s (0x%016" PRIx64 "); got: active "
               "0x%016" PRIx64 ", reached 0x%016" PRIx64 "\n",
               c->category, c->description, c->expected, c->want, got, reached);
        failures++;
    } else {
        report(true, c->category, c->description, c->expected);
    }
    ionic_eth_emu_destroy(emu);
}

/* Q_CONTROL through the BAR0 devcmd window acts the same. */
static void check_q_control_devcmd(void)
{
    struct ionic_eth_emu *emu = new_emu(NULL, NULL);
    uint8_t cmd[64] = {0};
    uint32_t doorbell = 1;
    uint8_t act[IONIC_EMU_ETH_QMAX];
    uint32_t n;

    q_init(emu, IONIC_QTYPE_RXQ, 2, 1, RING, CQ);
    cmd[0] = IONIC_CMD_Q_CONTROL;
    cmd[1] = IONIC_QTYPE_RXQ;
    ernic_put_le32(cmd + 4, 2);
    cmd[8] = IONIC_Q_ENABLE;
    ionic_eth_emu_bar0_access(emu, (char *)cmd, sizeof(cmd),
                              IONIC_BAR0_DEV_CMD_REGS_OFFSET + DEVCMD_CMD_OFF,
                              true);
    ionic_eth_emu_bar0_access(
        emu, (char *)&doorbell, sizeof(doorbell),
        IONIC_BAR0_DEV_CMD_REGS_OFFSET + DEVCMD_DOORBELL_OFF, true);
    n = ionic_eth_emu_rx_active(emu, act);
    report(n == 1 && act[0] == 2 &&
               emu->bar0[IONIC_BAR0_DEV_CMD_REGS_OFFSET + DEVCMD_COMP_OFF] ==
                   IONIC_RC_SUCCESS,
           "positive", "Q_CONTROL ENABLE of Rx queue 2 over the devcmd window",
           "status 0; queue 2 the one active queue");
    ionic_eth_emu_destroy(emu);
}

/* ---- Delivery to the steered queue -------------------------------------- */

#define RXQ_BASE    0x0400u
#define RXQ_SPAN    0x0100u /* per queue: ring, CQ, one buffer */
#define RXQ_RING(i) (RXQ_BASE + (i) * RXQ_SPAN)
#define RXQ_CQ(i)   (RXQ_RING(i) + 0x40u)
#define RXQ_BUF(i)  (RXQ_RING(i) + 0x80u)
#define RXQ_BUF_CAP 0x80u
#define RXQ_VEC(i)  (20u + (i))

_Static_assert(RXQ_RING(16) <= GUEST_MEM_SIZE, "16 Rx queues fit g_mem");

/* @n Rx queues, initialized and enabled, each with @posted (0 or 1) buffer
 * posted.  Vectors start masked, so each delivery latches intr_pending. */
static struct ionic_eth_emu *rx_emu(uint32_t n, uint32_t posted)
{
    struct ionic_eth_emu *emu = new_emu(NULL, ETH16);

    for (uint32_t i = 0; i < n; i++) {
        uint8_t *d = g_mem + RXQ_RING(i);

        q_init(emu, IONIC_QTYPE_RXQ, i, (uint16_t)RXQ_VEC(i), RXQ_RING(i),
               RXQ_CQ(i));
        q_control(emu, IONIC_QTYPE_RXQ, i, IONIC_Q_ENABLE);
        ernic_put_le16(d + 6, RXQ_BUF_CAP);
        ernic_put_le64(d + 8, RXQ_BUF(i));
        emu->eth_q[IONIC_QTYPE_RXQ][i].prod = (uint16_t)posted;
    }
    g_nwrites = 0;
    return emu;
}

/* A frame from @seed onward that steers to @want of the @n active queues. */
static size_t frame_for(uint8_t *f, const struct ionic_eth_emu *emu,
                        uint8_t want, uint32_t *seed)
{
    uint8_t act[IONIC_EMU_ETH_QMAX];
    uint32_t n = ionic_eth_emu_rx_active(emu, act);

    for (int i = 0; i < 100000; i++) {
        size_t len = random_frame(f, seed);

        if (n && ionic_eth_emu_rx_steer(act, n, f, len) == want)
            return len;
    }
    /* The row that wanted it then fails on the queue it lands on. */
    printf("FAIL setup: no tuple steers to queue %u\n", want);
    failures++;
    return random_frame(f, seed);
}

static bool cq_posted(uint32_t q)
{
    return (g_mem[RXQ_CQ(q) + ETH_COMP_SIZE - 1] & ADMIN_COMP_COLOR_MASK) != 0;
}

/*
 * The frame the fake TAP offers next.  Static, so the pointer left in
 * g_net_rx_frame never refers to a caller's stack.
 */
static uint8_t tap_buf[128];

static void tap_frame(struct ionic_eth_emu *emu, const uint8_t *f, size_t len)
{
    static int fake_net;

    if (len > sizeof(tap_buf))
        abort(); /* every frame here is built in 128 bytes */
    memcpy(tap_buf, f, len);
    emu->net = (struct ionic_eth_net *)&fake_net;
    g_net_rx_frame = tap_buf;
    g_net_rx_len = len;
    ionic_eth_emu_poll_rx(emu);
    emu->net = NULL;
}

static void check_delivery(void)
{
    struct ionic_eth_emu *emu = rx_emu(4, 1);
    uint8_t f1[128], f3[128];
    uint32_t seed = 11;
    size_t l1 = frame_for(f1, emu, 1, &seed);
    size_t l3 = frame_for(f3, emu, 3, &seed);

    tap_frame(emu, f1, l1);
    tap_frame(emu, f3, l3);
    report(cq_posted(1) && cq_posted(3) && !cq_posted(0) && !cq_posted(2) &&
               memcmp(g_mem + RXQ_BUF(1), f1, l1) == 0 &&
               memcmp(g_mem + RXQ_BUF(3), f3, l3) == 0,
           "positive", "two TAP flows steered to queues 1 and 3",
           "each frame in its queue's buffer and CQ; queues 0 and 2 untouched");
    report(emu->intr_pending[RXQ_VEC(1)] && emu->intr_pending[RXQ_VEC(3)] &&
               !emu->intr_pending[RXQ_VEC(0)] && !emu->intr_pending[RXQ_VEC(2)],
           "positive", "interrupts after the two flows",
           "raised on queues 1 and 3's vectors only");
    ionic_eth_emu_destroy(emu);

    emu = rx_emu(4, 1);
    seed = 12;
    l1 = frame_for(f1, emu, 2, &seed);
    ionic_eth_emu_queue_rx_frame(emu, f1, l1);
    ionic_eth_emu_poll_rx(emu);
    report(cq_posted(2) && !cq_posted(0) && emu->intr_pending[RXQ_VEC(2)] &&
               emu->inbox_count == 0,
           "positive", "an inbox (TCP mesh) frame for queue 2",
           "delivered to queue 2, not queue 0");
    ionic_eth_emu_destroy(emu);

    /* Queue 3 has no buffer while the others do: no fallback to another. */
    emu = rx_emu(4, 1);
    emu->eth_q[IONIC_QTYPE_RXQ][3].prod = 0;
    seed = 13;
    l3 = frame_for(f3, emu, 3, &seed);
    tap_frame(emu, f3, l3);
    report(!cq_posted(0) && !cq_posted(1) && !cq_posted(2) && !cq_posted(3) &&
               ionic_eth_emu_get_stats(emu)->lif[IONIC_LIF_RX_QUEUE_EMPTY] == 1,
           "negative", "a TAP frame steered to queue 3, which has no buffer",
           "dropped and counted in rx_queue_empty; no other queue takes it");
    ionic_eth_emu_destroy(emu);

    /* 16 queues, all with a buffer posted, then 8-15 disabled: a frame
     * steered over all 16 would land on a disabled queue. */
    emu = rx_emu(16, 1);
    for (uint32_t i = 8; i < 16; i++)
        q_control(emu, IONIC_QTYPE_RXQ, i, IONIC_Q_DISABLE);
    seed = 15;
    bool upper = false, lower = true;
    for (int i = 0; i < 256; i++) {
        l1 = random_frame(f1, &seed);
        tap_frame(emu, f1, l1);
    }
    for (uint32_t i = 0; i < 16; i++) {
        if (i < 8)
            lower = lower && cq_posted(i);
        else
            upper = upper || cq_posted(i);
    }
    report(lower && !upper &&
               ionic_eth_emu_get_stats(emu)->lif[IONIC_LIF_RX_QUEUE_EMPTY] ==
                   256 - 8,
           "corner", "256 TAP frames with 8 of 16 Rx queues enabled",
           "each of queues 0-7 takes one; none reaches 8-15, though they "
           "have buffers; the other 248 count as rx_queue_empty");
    ionic_eth_emu_destroy(emu);

    emu = rx_emu(4, 1);
    for (uint32_t i = 0; i < 4; i++)
        q_control(emu, IONIC_QTYPE_RXQ, i, IONIC_Q_DISABLE);
    seed = 14;
    l1 = random_frame(f1, &seed);
    tap_frame(emu, f1, l1);
    report(g_net_rx_frame != NULL && g_nwrites == 0, "negative",
           "a TAP frame with every Rx queue disabled",
           "the TAP is not read and nothing is written to the guest");
    g_net_rx_frame = NULL;
    ionic_eth_emu_destroy(emu);
}

/* Literal values pin the contract independently of ref_hash() above. */
static void check_hash_goldens(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        struct flow flow;
        size_t truncate;
        uint32_t hash;
    } cases[] = {
        {"positive", "fixed IPv4 TCP tuple", "0xab8b421d", FL_TCP4(), 0,
         0xab8b421du},
        {"positive", "fixed IPv6 UDP tuple", "0x4517655f", FL_UDP6(), 0,
         0x4517655fu},
        {"corner", "VLAN preserves IPv4 tuple", "0xab8b421d",
         FL_TCP4(.tags = {0x8100}), 0, 0xab8b421du},
        {"corner", "fragment excludes ports", "0x2b767992",
         FL_TCP4(.frag = 0x2000), 0, 0x2b767992u},
        {"negative", "invalid IPv4 version falls back to MACs", "0x4babd2e8",
         FL_TCP4(.version = 6), 0, 0x4babd2e8u},
        {"boundary", "exactly twelve MAC bytes", "0x4babd2e8", FL_TCP4(), 12,
         0x4babd2e8u},
        {"boundary", "eleven bytes cannot hold MACs", "0x811c9dc5", FL_TCP4(),
         11, 0x811c9dc5u},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t frame[128];
        size_t l3, l4;
        size_t len = build_flow(frame, &cases[i].flow, &l3, &l4);

        if (cases[i].truncate)
            len = cases[i].truncate;
        report(ionic_eth_flow_hash(frame, len) == cases[i].hash,
               cases[i].category, cases[i].description, cases[i].expected);
    }
}

static void check_ring_wrap(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        unsigned int packets;
    } cases[] = {
        {"positive", "one completion", "index 1, initial color retained", 1},
        {"boundary", "exactly one ring", "indices zero, color toggled", 4},
        {"corner", "two wraps plus one", "index 1, original color restored", 9},
    };
    for (size_t row = 0; row < sizeof(cases) / sizeof(cases[0]); row++) {
        struct ionic_eth_emu *emu = rx_emu(2, 0);
        struct eth_queue *q = &emu->eth_q[IONIC_QTYPE_RXQ][1];
        uint8_t frame[128];
        uint32_t seed = 101;
        size_t len = frame_for(frame, emu, 1, &seed);
        bool ok = true;

        for (unsigned int slot = 1; slot < 4; slot++)
            memcpy(g_mem + RXQ_RING(1) + slot * ETH_DESC_SIZE,
                   g_mem + RXQ_RING(1), ETH_DESC_SIZE);
        for (unsigned int n = 0; n < cases[row].packets; n++) {
            unsigned int slot = n % 4;
            uint8_t *comp = g_mem + RXQ_CQ(1) + slot * ETH_COMP_SIZE;

            q->prod = (uint16_t)((q->head + 1) % 4);
            tap_frame(emu, frame, len);
            ok = ok && ernic_get_le16(comp + 2) == slot &&
                 ernic_get_le16(comp + 12) == len &&
                 !!(comp[15] & ADMIN_COMP_COLOR_MASK) == ((n / 4) % 2 == 0) &&
                 q->head == (n + 1) % 4 && q->cq_index == (n + 1) % 4 &&
                 memcmp(g_mem + RXQ_BUF(1), frame, len) == 0;
        }
        ok = ok && !cq_posted(0) && emu->eth_q[IONIC_QTYPE_RXQ][0].head == 0 &&
             q->cq_color == ((cases[row].packets / 4) % 2 == 0);
        report(ok, cases[row].category, cases[row].description,
               cases[row].expected);
        ionic_eth_emu_destroy(emu);
    }
}

static void check_held_frame_transition(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        uint32_t target;
        bool pause_all;
    } cases[] = {
        {"positive", "buffer arrives on original queue",
         "held frame advances once; next frame drops on event 2", 3, false},
        {"corner", "original queue disabled while frame held",
         "re-steer to queue 2 without advancing twice; next frame drops", 2,
         false},
        {"negative", "all queues disabled while frame held",
         "pause preserves pattern; deliver once after enable, next frame drops",
         3, true},
    };
    for (size_t row = 0; row < sizeof(cases) / sizeof(cases[0]); row++) {
        struct ionic_eth_emu *emu = rx_emu(4, 0);
        struct ionic_fault_set *set = ionic_fault_set_new();
        struct ionic_fault *fault;
        char err[128];
        uint8_t frame[128];
        uint32_t seed = 111;
        size_t len = frame_for(frame, emu, 3, &seed);
        bool ok;

        ok = set && ionic_fault_set_add(set, "eth_rx: after 2 effect drop", err,
                                        sizeof(err)) == 0;
        ionic_eth_emu_set_faults(emu, set);
        fault = ionic_fault_set_get(set, IONIC_FAULT_SITE_ETH_RX);
        ionic_eth_emu_queue_rx_frame(emu, frame, len);
        ionic_eth_emu_poll_rx(emu);
        ok = ok && emu->inbox_count == 1 && g_nwrites == 0;
        for (uint32_t q = 0; q < 4; q++)
            q_control(emu, IONIC_QTYPE_RXQ, q, IONIC_Q_DISABLE);
        if (cases[row].pause_all) {
            ionic_eth_emu_poll_rx(emu);
            ok = ok && emu->inbox_count == 1 && g_nwrites == 0;
        }
        q_control(emu, IONIC_QTYPE_RXQ, cases[row].target, IONIC_Q_ENABLE);
        emu->eth_q[IONIC_QTYPE_RXQ][cases[row].target].prod = 1;
        ionic_eth_emu_poll_rx(emu);
        ionic_eth_emu_poll_rx(emu);
        ok = ok && emu->inbox_count == 0 && g_nwrites == 2 &&
             memcmp(g_mem + RXQ_BUF(cases[row].target), frame, len) == 0 &&
             ionic_eth_emu_get_stats(emu)->lif[IONIC_LIF_RX_QUEUE_EMPTY] == 0;
        for (uint32_t q = 0; q < 4; q++)
            ok = ok && cq_posted(q) == (q == cases[row].target);
        ok = ok && fault && fault->eligible == 1 && fault->fired == 0;
        ionic_eth_emu_queue_rx_frame(emu, frame, len);
        ionic_eth_emu_poll_rx(emu);
        ok = ok && fault && fault->eligible == 2 && fault->fired == 1 &&
             emu->inbox_count == 0 && g_nwrites == 2;
        report(ok, cases[row].category, cases[row].description,
               cases[row].expected);
        ionic_eth_emu_destroy(emu);
        ionic_fault_set_free(set);
    }
}

static void check_fault_poll_budget(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        unsigned int frames;
    } rows[] = {
        {"negative", "empty inbox", "no eligible events", 0},
        {"positive", "one frame", "one eligible event and drop", 1},
        {"boundary", "one below poll limit", "63 events in first poll", 63},
        {"boundary", "exact poll limit", "64 events in first poll", 64},
        {"corner", "one above poll limit", "64 events, then one on next poll",
         65},
    };
    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        struct ionic_eth_emu *emu = rx_emu(2, 0);
        struct ionic_fault_set *set = ionic_fault_set_new();
        struct ionic_fault *rx;
        uint8_t frame[128];
        uint32_t seed = 141;
        size_t len = frame_for(frame, emu, 1, &seed);
        char err[128];
        unsigned int first = rows[row].frames > 64 ? 64 : rows[row].frames;
        bool ok = set && ionic_fault_set_add(set, "eth_rx: every 1 effect drop",
                                             err, sizeof(err)) == 0;

        ionic_eth_emu_set_faults(emu, set);
        rx = ionic_fault_set_get(set, IONIC_FAULT_SITE_ETH_RX);
        for (unsigned int n = 0; n < rows[row].frames; n++)
            ok = ionic_eth_emu_queue_rx_frame(emu, frame, len) == 0 && ok;
        ionic_eth_emu_poll_rx(emu);
        ok = ok && rx && rx->eligible == first && rx->fired == first &&
             emu->inbox_count == rows[row].frames - first && g_nwrites == 0;
        ionic_eth_emu_poll_rx(emu);
        ok = ok && rx && rx->eligible == rows[row].frames &&
             rx->fired == rows[row].frames && emu->inbox_count == 0 &&
             g_nwrites == 0;
        report(ok, rows[row].category, rows[row].description,
               rows[row].expected);
        ionic_eth_emu_destroy(emu);
        ionic_fault_set_free(set);
    }
}

static void check_two_queue_faults(void)
{
    struct ionic_eth_emu *emu = rx_emu(2, 1);
    struct ionic_fault_set *set = ionic_fault_set_new();
    struct ionic_fault *rx, *tx;
    uint8_t frame[128];
    uint32_t seed = 151;
    char err[128];
    bool ok = set &&
              ionic_fault_set_add(set, "eth_rx: every 2 effect drop", err,
                                  sizeof(err)) == 0 &&
              ionic_fault_set_add(set, "eth_tx: every 3 effect drop", err,
                                  sizeof(err)) == 0;
    static const uint8_t destinations[] = {0, 1, 1};

    ionic_eth_emu_set_faults(emu, set);
    rx = ionic_fault_set_get(set, IONIC_FAULT_SITE_ETH_RX);
    tx = ionic_fault_set_get(set, IONIC_FAULT_SITE_ETH_TX);
    for (size_t i = 0; i < sizeof(destinations); i++) {
        size_t len = frame_for(frame, emu, destinations[i], &seed);

        tap_frame(emu, frame, len);
    }
    ok = ok && rx && tx && rx->eligible == 3 && rx->fired == 1 &&
         tx->eligible == 0 && cq_posted(0) && cq_posted(1) &&
         emu->eth_q[IONIC_QTYPE_RXQ][0].head == 1 &&
         emu->eth_q[IONIC_QTYPE_RXQ][1].head == 1;
    q_init(emu, IONIC_QTYPE_TXQ, 1, 22, 0x100, 0x200);
    q_control(emu, IONIC_QTYPE_TXQ, 1, IONIC_Q_ENABLE);
    for (uint16_t i = 0; i < 3; i++) {
        uint8_t *desc = g_mem + 0x100 + (size_t)i * ETH_DESC_SIZE;
        /* p_index i + 1 in bits 15:0, qid 1 in bits 31:24. */
        uint64_t db = (uint64_t)(i + 1) | (UINT64_C(1) << 24);

        memcpy(g_mem + 0x300, frame, 64);
        ernic_put_le64(desc, (uint64_t)0x300 << TXQ_DESC_ADDR_SHIFT);
        ernic_put_le16(desc + 8, 64);
        ionic_eth_emu_bar2_access(emu, (char *)&db, sizeof(db),
                                  (loff_t)IONIC_QTYPE_TXQ * 8, true);
    }
    ok = ok && rx && tx && rx->eligible == 3 && rx->fired == 1 &&
         tx->eligible == 3 && tx->fired == 1 &&
         emu->eth_q[IONIC_QTYPE_TXQ][1].head == 3 &&
         emu->eth_q[IONIC_QTYPE_TXQ][0].head == 0;
    report(ok, "corner", "interleaved flows on two RX queues and TX queue 1",
           "RX pattern shared across queues; drops consume no RX buffer; TX "
           "advances independently and completes drops");
    ionic_eth_emu_destroy(emu);
    ionic_fault_set_free(set);
}

int main(void)
{
    for (size_t i = 0; i < sizeof(shape_cases) / sizeof(shape_cases[0]); i++)
        run_shape_case(&shape_cases[i]);
    for (size_t i = 0; i < sizeof(qinit_cases) / sizeof(qinit_cases[0]); i++)
        run_qinit_case(&qinit_cases[i]);
    check_txq_past_count();
    check_vectors();
    for (size_t i = 0; i < sizeof(hash_cases) / sizeof(hash_cases[0]); i++)
        run_hash_case(&hash_cases[i]);
    check_steer_basic();
    for (size_t i = 0; i < sizeof(active_cases) / sizeof(active_cases[0]); i++)
        run_active_case(&active_cases[i]);
    check_q_control_devcmd();
    check_delivery();
    check_fault_poll_budget();
    check_two_queue_faults();
    check_hash_goldens();
    check_ring_wrap();
    check_held_frame_transition();

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
