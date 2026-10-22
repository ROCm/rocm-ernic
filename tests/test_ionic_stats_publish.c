/*
 * Unit tests for the ionic emulator's LIF statistics: what the datapath
 * counts, and when and where the block is written into guest memory.
 *
 * ionic_get_stats64() reads struct ionic_lif_stats out of the LIF info
 * block LIF_INIT pointed at, so the device must write it there, keep it
 * current, and stop the moment the guest may have freed it (LIF_RESET or a
 * device reset).  The test links the emulator, reaches its state through
 * ionic_eth_emu_priv.h, and backs guest memory with a plain buffer
 * (ionic_eth_emu_fakes.h), so each row drives the real devcmd, doorbell and
 * receive paths with no guest.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "ionic_emu_stats.h"
#include "ionic_eth_emu.h"
#include "ionic_eth_emu_fakes.h"
#include "ionic_eth_emu_priv.h"
#include "ionic_profile.h"
#include "parse_int.h"

/* ---- Guest memory layout ----------------------------------------------- */

#define POISON 0xa5u

#define LIF_PA     0x0100u /* not 0: a zero address means "no LIF" */
#define LIF_PA2    0x1800u /* a second info block, for re-init */
#define TX_RING    0x0800u
#define TX_CQ      0x0900u
#define TX_BUF     0x0a00u
#define RX_RING    0x0c00u
#define RX_CQ      0x0d00u
#define RX_BUF     0x0e00u /* two buffers of RX_BUF_CAP */
#define RX_BUF_CAP 1024u
#define Q_DEPTH    4u

_Static_assert(LIF_PA + IONIC_LIF_INFO_STATS_OFF + IONIC_LIF_STATS_BYTES <=
                   TX_RING,
               "the first info block overlaps the Tx ring");
_Static_assert(RX_BUF + 2 * RX_BUF_CAP <= LIF_PA2,
               "the Rx buffers overlap the second info block");
_Static_assert(LIF_PA2 + IONIC_LIF_INFO_STATS_OFF + IONIC_LIF_STATS_BYTES <=
                   GUEST_MEM_SIZE,
               "the second info block is past the end of guest memory");

static int failures;

/* ---- Helpers ------------------------------------------------------------ */

static const uint8_t mac_ucast[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
static const uint8_t mac_bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static uint8_t g_frame[2048];

static const uint8_t *make_frame(const uint8_t dst[6], size_t len)
{
    memset(g_frame, 0x3c, sizeof(g_frame));
    memcpy(g_frame, dst, 6);
    return g_frame;
}

/* A fresh device, guest memory poisoned, every fake reset. */
static struct ionic_eth_emu *new_emu(uint32_t interval_ms)
{
    struct ionic_eth_emu *emu = ionic_eth_emu_create(NULL, 4096);
    struct ionic_profile p;

    if (!emu) {
        fprintf(stderr, "ionic_eth_emu_create failed\n");
        /* Every row needs a device; abort() rather than exit(), which
         * isn't thread safe. */
        abort();
    }

    memset(g_mem, POISON, sizeof(g_mem));
    g_nwrites = 0;
    g_write_attempts = 0;
    g_fail_writes = 0;
    g_nwarnings = 0;
    g_net_send_rc = 0;
    g_net_rx_frame = NULL;
    g_net_rx_len = 0;
    ionic_profile_defaults(&p);
    p.stats.interval_ms = interval_ms;
    ionic_eth_emu_set_profile(emu, &p);
    return emu;
}

/*
 * Issue devcmd @opcode with @info_pa at byte 8, as LIF_INIT lays it out, and
 * ring the dev_cmd doorbell as the driver does.
 */
static void devcmd(struct ionic_eth_emu *emu, uint8_t opcode, uint64_t info_pa)
{
    uint8_t cmd[64];
    uint64_t pa = htole64(info_pa);
    uint32_t doorbell = 1;

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = opcode;
    memcpy(cmd + 8, &pa, 8);
    ionic_eth_emu_bar0_access(emu, (char *)cmd, sizeof(cmd),
                              IONIC_BAR0_DEV_CMD_REGS_OFFSET + DEVCMD_CMD_OFF,
                              true);
    ionic_eth_emu_bar0_access(
        emu, (char *)&doorbell, sizeof(doorbell),
        IONIC_BAR0_DEV_CMD_REGS_OFFSET + DEVCMD_DOORBELL_OFF, true);
}

static void lif_init(struct ionic_eth_emu *emu, uint64_t pa)
{
    devcmd(emu, IONIC_CMD_LIF_INIT, pa);
    /* LIF_INIT writes the link status; rows count from here. */
    g_nwrites = 0;
}

static uint64_t guest_qword(uint64_t pa, unsigned int idx)
{
    uint64_t v;

    memcpy(&v, g_mem + pa + IONIC_LIF_INFO_STATS_OFF + (size_t)idx * 8, 8);
    return le64toh(v);
}

static uint64_t counter(struct ionic_eth_emu *emu, unsigned int idx)
{
    const struct ionic_emu_stats *st = ionic_eth_emu_get_stats(emu);

    if (!st)
        abort(); /* only a NULL emu has no stats */
    return st->lif[idx];
}

/* Bytes in [@pa, @pa + @len) still holding POISON. */
static size_t poisoned(uint64_t pa, size_t len)
{
    size_t n = 0;

    for (size_t i = 0; i < len; i++)
        n += g_mem[pa + i] == POISON;
    return n;
}

static void setup_txq(struct ionic_eth_emu *emu)
{
    struct eth_queue *q = &emu->eth_q[IONIC_QTYPE_TXQ][0];

    q->valid = true;
    q->ring_base = TX_RING;
    q->cq_ring_base = TX_CQ;
    q->depth = Q_DEPTH;
}

/* Ring Tx queue 0's doorbell: p_index in bits 15:0, qid 0 above them. */
static void tx_doorbell(struct ionic_eth_emu *emu, uint16_t p_index)
{
    uint64_t db = p_index;

    ionic_eth_emu_bar2_access(emu, (char *)&db, sizeof(db),
                              (loff_t)IONIC_QTYPE_TXQ * 8, true);
}

/* Post one Tx descriptor for @len bytes at @addr and ring the doorbell. */
static void tx_one(struct ionic_eth_emu *emu, uint64_t addr, uint16_t len)
{
    const struct eth_queue *q = &emu->eth_q[IONIC_QTYPE_TXQ][0];
    uint8_t *d = g_mem + TX_RING + (size_t)q->head * ETH_DESC_SIZE;
    uint64_t cmd = htole64(addr << TXQ_DESC_ADDR_SHIFT);
    uint16_t l = htole16(len);

    memset(d, 0, ETH_DESC_SIZE);
    memcpy(d, &cmd, 8);
    memcpy(d + 8, &l, 2);
    tx_doorbell(emu, (uint16_t)(q->head + 1));
}

static void tx_frame(struct ionic_eth_emu *emu, const uint8_t dst[6],
                     uint16_t len)
{
    memcpy(g_mem + TX_BUF, make_frame(dst, len), len);
    tx_one(emu, TX_BUF, len);
}

/* An Rx queue with @posted buffers of @cap bytes. */
static void setup_rxq(struct ionic_eth_emu *emu, uint16_t posted, uint16_t cap)
{
    struct eth_queue *q = &emu->eth_q[IONIC_QTYPE_RXQ][0];

    q->valid = true;
    q->enabled =
        true; /* the only enabled Rx queue, so every frame lands here */
    q->ring_base = RX_RING;
    q->cq_ring_base = RX_CQ;
    q->depth = Q_DEPTH;
    q->prod = posted;
    for (uint16_t i = 0; i < posted && i < 2; i++) {
        uint8_t *d = g_mem + RX_RING + (size_t)i * ETH_DESC_SIZE;
        uint16_t c = htole16(cap);
        uint64_t a = htole64(RX_BUF + (uint64_t)i * RX_BUF_CAP);

        memset(d, 0, ETH_DESC_SIZE);
        memcpy(d + 6, &c, 2);
        memcpy(d + 8, &a, 8);
    }
}

/* A frame arriving on the host TAP, then one receive poll. */
static void rx_tap(struct ionic_eth_emu *emu, const uint8_t dst[6], size_t len)
{
    static int fake_net;

    emu->net = (struct ionic_eth_net *)&fake_net;
    g_net_rx_frame = make_frame(dst, len);
    g_net_rx_len = len;
    ionic_eth_emu_poll_rx(emu);
}

/* ---- Rows --------------------------------------------------------------- */

/*
 * Each row runs one scenario on a fresh device and fills @got with what it
 * saw; it passes when that equals @expected exactly.
 */
struct publish_case {
    const char *category;
    const char *description;
    const char *expected;
    void (*run)(char *got, size_t len);
};

static void lif_init_then_publish(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    lif_init(emu, LIF_PA);
    memset(g_mem, POISON, sizeof(g_mem));
    ionic_eth_emu_publish_stats(emu, 5000);
    snprintf(got, len, "writes %u, block poisoned %zu, before %zu, after %zu",
             g_nwrites,
             poisoned(LIF_PA + IONIC_LIF_INFO_STATS_OFF, IONIC_LIF_STATS_BYTES),
             poisoned(LIF_PA, IONIC_LIF_INFO_STATS_OFF),
             poisoned(LIF_PA + IONIC_LIF_INFO_STATS_OFF + IONIC_LIF_STATS_BYTES,
                      64));
    ionic_eth_emu_destroy(emu);
}

static void traffic_then_publish(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    lif_init(emu, LIF_PA);
    setup_txq(emu);
    setup_rxq(emu, 2, RX_BUF_CAP);
    tx_frame(emu, mac_ucast, 100);
    tx_frame(emu, mac_ucast, 200);
    rx_tap(emu, mac_bcast, 60);
    ionic_eth_emu_publish_stats(emu, 5000);
    snprintf(got, len,
             "tx_ucast %" PRIu64 "/%" PRIu64 ", rx_bcast %" PRIu64 "/%" PRIu64,
             guest_qword(LIF_PA, IONIC_LIF_TX_UCAST_PACKETS),
             guest_qword(LIF_PA, IONIC_LIF_TX_UCAST_BYTES),
             guest_qword(LIF_PA, IONIC_LIF_RX_BCAST_PACKETS),
             guest_qword(LIF_PA, IONIC_LIF_RX_BCAST_BYTES));
    ionic_eth_emu_destroy(emu);
}

/* Writes made by publishing at each of @times, after LIF_INIT. */
static unsigned int publish_at(uint32_t interval_ms, const uint64_t *times,
                               size_t n)
{
    struct ionic_eth_emu *emu = new_emu(interval_ms);

    lif_init(emu, LIF_PA);
    for (size_t i = 0; i < n; i++)
        ionic_eth_emu_publish_stats(emu, times[i]);
    ionic_eth_emu_destroy(emu);
    return g_nwrites;
}

static void interval_elapsed(char *got, size_t len)
{
    static const uint64_t t[] = {5000, 5500, 6000};

    snprintf(got, len, "writes %u", publish_at(1000, t, 3));
}

static void interval_9_of_10(char *got, size_t len)
{
    static const uint64_t t[] = {100, 109};

    snprintf(got, len, "writes %u", publish_at(10, t, 2));
}

static void interval_10_of_10(char *got, size_t len)
{
    static const uint64_t t[] = {100, 110};

    snprintf(got, len, "writes %u", publish_at(10, t, 2));
}

static void clock_jump(char *got, size_t len)
{
    static const uint64_t t[] = {100, 200};

    snprintf(got, len, "writes %u", publish_at(10, t, 2));
}

static void before_lif_init(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(10);

    ionic_eth_emu_publish_stats(emu, 0);
    ionic_eth_emu_publish_stats(emu, 1000);
    snprintf(got, len, "writes %u", g_nwrites);
    ionic_eth_emu_destroy(emu);
}

static void lif_init_no_pa(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(10);

    lif_init(emu, 0);
    ionic_eth_emu_publish_stats(emu, 1000);
    snprintf(got, len, "writes %u", g_nwrites);
    ionic_eth_emu_destroy(emu);
}

static void after_lif_reset(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(10);

    lif_init(emu, LIF_PA);
    ionic_eth_emu_publish_stats(emu, 100);
    devcmd(emu, IONIC_CMD_LIF_RESET, 0);
    g_nwrites = 0;
    ionic_eth_emu_publish_stats(emu, 1000);
    ionic_eth_emu_publish_stats(emu, 2000);
    snprintf(got, len, "writes %u", g_nwrites);
    ionic_eth_emu_destroy(emu);
}

static void after_device_reset(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(10);

    lif_init(emu, LIF_PA);
    setup_txq(emu);
    tx_frame(emu, mac_ucast, 100);
    devcmd(emu, IONIC_CMD_RESET, 0);
    g_nwrites = 0;
    ionic_eth_emu_publish_stats(emu, 1000);
    snprintf(got, len, "writes %u, tx_ucast_packets %" PRIu64, g_nwrites,
             counter(emu, IONIC_LIF_TX_UCAST_PACKETS));
    ionic_eth_emu_destroy(emu);
}

static void reinit_new_pa(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    lif_init(emu, LIF_PA);
    ionic_eth_emu_publish_stats(emu, 100);
    lif_init(emu, LIF_PA2);
    memset(g_mem, POISON, sizeof(g_mem));
    /* Within the interval: LIF_INIT makes the next tick write anyway. */
    ionic_eth_emu_publish_stats(emu, 101);
    snprintf(
        got, len, "writes %u, old poisoned %zu, new poisoned %zu", g_nwrites,
        poisoned(LIF_PA + IONIC_LIF_INFO_STATS_OFF, IONIC_LIF_STATS_BYTES),
        poisoned(LIF_PA2 + IONIC_LIF_INFO_STATS_OFF, IONIC_LIF_STATS_BYTES));
    ionic_eth_emu_destroy(emu);
}

static void write_fails(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(10);
    uint64_t before;

    lif_init(emu, LIF_PA);
    setup_txq(emu);
    tx_frame(emu, mac_ucast, 100);
    before = counter(emu, IONIC_LIF_TX_UCAST_PACKETS);
    g_nwrites = 0;
    g_nwarnings = 0;
    g_fail_writes = 2;
    ionic_eth_emu_publish_stats(emu, 100); /* fails, logged */
    ionic_eth_emu_publish_stats(emu, 110); /* fails, not logged again */
    ionic_eth_emu_publish_stats(emu, 120); /* retried: works */
    snprintf(got, len,
             "writes %u, warnings %u, counter unchanged %d, guest "
             "tx_ucast_packets %" PRIu64,
             g_nwrites, g_nwarnings,
             counter(emu, IONIC_LIF_TX_UCAST_PACKETS) == before,
             guest_qword(LIF_PA, IONIC_LIF_TX_UCAST_PACKETS));
    ionic_eth_emu_destroy(emu);
}

static void reserved_zero(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(10);
    unsigned int bad = 0;

    lif_init(emu, LIF_PA);
    /* Junk in every slot, named or not, must not reach a reserved one. */
    for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++)
        emu->stats.lif[q] = 0x0101010101010101ull * (q + 1);
    ionic_eth_emu_publish_stats(emu, 100);
    for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++)
        if (!ionic_emu_lif_stat_name(q) && guest_qword(LIF_PA, q) != 0)
            bad++;
    snprintf(got, len, "nonzero reserved qwords %u", bad);
    ionic_eth_emu_destroy(emu);
}

static void address_boundaries(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        uint64_t base;
        unsigned int attempts;
        unsigned int writes;
    } cases[] = {
        {"positive", "ordinary registered address",
         "one mapping and exact block write", LIF_PA, 1, 1},
        {"positive", "unaligned address", "unaligned DMA is supported",
         LIF_PA + 1, 1, 1},
        {"negative", "zero means unregistered", "no mapping or guest write", 0,
         0, 0},
        {"boundary", "smallest nonzero address",
         "one mapping and exact block write", 1, 1, 1},
        {"boundary", "block ends one byte before guest-memory end",
         "one trailing byte preserved",
         GUEST_MEM_SIZE - IONIC_LIF_INFO_STATS_OFF - IONIC_LIF_STATS_BYTES - 1,
         1, 1},
        {"boundary", "block ends exactly at guest-memory end",
         "last byte written; prefix preserved",
         GUEST_MEM_SIZE - IONIC_LIF_INFO_STATS_OFF - IONIC_LIF_STATS_BYTES, 1,
         1},
        {"negative", "one byte beyond guest memory",
         "mapping rejected; all guest bytes preserved",
         GUEST_MEM_SIZE - IONIC_LIF_INFO_STATS_OFF - IONIC_LIF_STATS_BYTES + 1,
         1, 0},
        /* ernic_dma_range_ok() needs gpa + len to fit in 64 bits, so the
         * last block that reaches the mapping ends at UINT64_MAX. */
        {"boundary", "block ends at UINT64_MAX",
         "valid arithmetic; unmapped range rejected",
         UINT64_MAX - IONIC_LIF_INFO_STATS_OFF - IONIC_LIF_STATS_BYTES, 1, 0},
        {"boundary", "block end overflows by one byte",
         "reject before DMA; all guest bytes preserved",
         UINT64_MAX - IONIC_LIF_INFO_STATS_OFF - IONIC_LIF_STATS_BYTES + 1, 0,
         0},
        {"boundary", "offset reaches UINT64_MAX",
         "block cannot fit; reject before DMA",
         UINT64_MAX - IONIC_LIF_INFO_STATS_OFF, 0, 0},
        {"corner", "offset wraps to guest address zero",
         "reject before DMA; address zero untouched",
         UINT64_MAX - IONIC_LIF_INFO_STATS_OFF + 1, 0, 0},
        {"corner", "maximum base address",
         "reject before DMA; all guest bytes preserved", UINT64_MAX, 0, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct ionic_eth_emu *emu = new_emu(10);
        bool ok;

        /* Isolate stats DMA from LIF_INIT's separately tested status DMA. */
        emu->lif_info_pa = cases[i].base;
        emu->stats_publish_now = true;
        emu->stats.lif[IONIC_LIF_TX_UCAST_PACKETS] = 7;
        ionic_eth_emu_publish_stats(emu, 100);
        ok = g_write_attempts == cases[i].attempts &&
             g_nwrites == cases[i].writes;
        if (!cases[i].writes) {
            ok = ok && poisoned(0, sizeof(g_mem)) == sizeof(g_mem);
        } else {
            uint64_t start = cases[i].base + IONIC_LIF_INFO_STATS_OFF;
            size_t end = (size_t)start + IONIC_LIF_STATS_BYTES;

            ok = ok &&
                 guest_qword(cases[i].base, IONIC_LIF_TX_UCAST_PACKETS) == 7 &&
                 poisoned(0, (size_t)start) == start &&
                 poisoned(end, sizeof(g_mem) - end) == sizeof(g_mem) - end;
        }
        /* A second tick retries failures without corrupting counters. */
        ionic_eth_emu_publish_stats(emu, 110);
        ok = ok && g_write_attempts == 2 * cases[i].attempts &&
             g_nwrites == 2 * cases[i].writes;
        lif_init(emu, LIF_PA);
        g_write_attempts = 0;
        ionic_eth_emu_publish_stats(emu, 120);
        ok = ok && g_write_attempts == 1 && g_nwrites == 1 &&
             guest_qword(LIF_PA, IONIC_LIF_TX_UCAST_PACKETS) == 7;
        printf("%s %s: %s; expected: %s; retry and re-registration retain 7 "
               "packets\n",
               ok ? "ok" : "FAIL", cases[i].category, cases[i].description,
               cases[i].expected);
        if (!ok)
            failures++;
        ionic_eth_emu_destroy(emu);
    }
}

static void lif_status_address_boundaries(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        uint64_t base;
        unsigned int attempts;
        unsigned int writes;
    } cases[] = {
        {"positive", "normal LIF_INIT", "one status block written", LIF_PA, 1,
         1},
        {"negative", "zero LIF_INIT address", "no DMA", 0, 0, 0},
        {"boundary", "status ends exactly at guest-memory end",
         "one status block written",
         GUEST_MEM_SIZE - IONIC_LIF_INFO_STATUS_OFF - 64, 1, 1},
        {"negative", "status ends one byte past guest memory",
         "mapping fails; guest memory unchanged",
         GUEST_MEM_SIZE - IONIC_LIF_INFO_STATUS_OFF - 63, 1, 0},
        {"boundary", "status ends at UINT64_MAX",
         "valid arithmetic; unmapped range rejected",
         UINT64_MAX - IONIC_LIF_INFO_STATUS_OFF - 64, 1, 0},
        {"boundary", "status end overflows by one", "reject before DMA",
         UINT64_MAX - IONIC_LIF_INFO_STATUS_OFF - 63, 0, 0},
        {"corner", "status offset wraps to zero",
         "reject before DMA; address zero untouched",
         UINT64_MAX - IONIC_LIF_INFO_STATUS_OFF + 1, 0, 0},
        {"corner", "maximum base address", "reject before DMA", UINT64_MAX, 0,
         0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct ionic_eth_emu *emu = new_emu(10);
        bool ok;

        devcmd(emu, IONIC_CMD_LIF_INIT, cases[i].base);
        ok = g_write_attempts == cases[i].attempts &&
             g_nwrites == cases[i].writes;
        if (!cases[i].writes)
            ok = ok && poisoned(0, sizeof(g_mem)) == sizeof(g_mem);
        printf("%s %s: %s; expected: %s\n", ok ? "ok" : "FAIL",
               cases[i].category, cases[i].description, cases[i].expected);
        if (!ok)
            failures++;
        ionic_eth_emu_destroy(emu);
    }
}

static const struct publish_case publish_cases[] = {
    {"positive", "LIF_INIT(pa), then publish",
     "writes 1, block poisoned 0, before 320, after 64", lif_init_then_publish},
    {"positive", "Tx 100 + 200 bytes unicast, Rx 60 bytes broadcast, publish",
     "tx_ucast 2/300, rx_bcast 1/60", traffic_then_publish},
    {"positive", "interval 1000: publish at 0, +500, +1000 ms", "writes 2",
     interval_elapsed},
    {"negative", "publish before LIF_INIT", "writes 0", before_lif_init},
    {"negative", "LIF_INIT with info_pa 0", "writes 0", lif_init_no_pa},
    {"negative", "LIF_INIT, LIF_RESET, then publish twice", "writes 0",
     after_lif_reset},
    {"negative", "traffic, device reset, then publish",
     "writes 0, tx_ucast_packets 0", after_device_reset},
    {"boundary", "interval 10: the clock advances 9 ms", "writes 1",
     interval_9_of_10},
    {"boundary", "interval 10: the clock advances exactly 10 ms", "writes 2",
     interval_10_of_10},
    {"boundary", "interval 10: the clock jumps 100 ms",
     "writes 2 (one, not ten, for the jump)", clock_jump},
    {"corner", "LIF_INIT again with a new pa, inside the interval",
     "writes 1, old poisoned 1024, new poisoned 0", reinit_new_pa},
    {"corner", "the stats write fails twice, then works",
     "writes 1, warnings 1, counter unchanged 1, guest tx_ucast_packets 1",
     write_fails},
    {"corner", "junk in reserved slots of the counter block",
     "nonzero reserved qwords 0", reserved_zero},
};

/* ---- Datapath rows ------------------------------------------------------ */

static void tx_sink(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    setup_txq(emu);
    tx_frame(emu, mac_ucast, 100);
    snprintf(got, len, "tx_ucast %" PRIu64 "/%" PRIu64,
             counter(emu, IONIC_LIF_TX_UCAST_PACKETS),
             counter(emu, IONIC_LIF_TX_UCAST_BYTES));
    ionic_eth_emu_destroy(emu);
}

static void tx_send_fails(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    static int fake_net;

    setup_txq(emu);
    emu->net = (struct ionic_eth_net *)&fake_net;
    g_net_send_rc = -EAGAIN;
    tx_frame(emu, mac_ucast, 100);
    snprintf(got, len,
             "tx_ucast %" PRIu64 ", tx_ucast_drop %" PRIu64 "/%" PRIu64,
             counter(emu, IONIC_LIF_TX_UCAST_PACKETS),
             counter(emu, IONIC_LIF_TX_UCAST_DROP_PACKETS),
             counter(emu, IONIC_LIF_TX_UCAST_DROP_BYTES));
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
}

static void tx_disabled(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    tx_doorbell(emu, 1);
    snprintf(got, len, "tx_queue_disabled %" PRIu64,
             counter(emu, IONIC_LIF_TX_QUEUE_DISABLED));
    ionic_eth_emu_destroy(emu);
}

static void tx_len_zero(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    setup_txq(emu);
    tx_one(emu, TX_BUF, 0);
    snprintf(got, len, "tx_desc_data_error %" PRIu64 ", head %u",
             counter(emu, IONIC_LIF_TX_DESC_DATA_ERROR),
             (unsigned)emu->eth_q[IONIC_QTYPE_TXQ][0].head);
    ionic_eth_emu_destroy(emu);
}

static void tx_bad_addr(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    setup_txq(emu);
    tx_one(emu, GUEST_MEM_SIZE + 0x1000u, 64);
    snprintf(got, len, "tx_dma_error %" PRIu64 ", tx_ucast %" PRIu64,
             counter(emu, IONIC_LIF_TX_DMA_ERROR),
             counter(emu, IONIC_LIF_TX_UCAST_PACKETS));
    ionic_eth_emu_destroy(emu);
}

static void rx_tap_ok(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    setup_rxq(emu, 1, RX_BUF_CAP);
    rx_tap(emu, mac_ucast, 64);
    snprintf(got, len, "rx_ucast %" PRIu64 "/%" PRIu64,
             counter(emu, IONIC_LIF_RX_UCAST_PACKETS),
             counter(emu, IONIC_LIF_RX_UCAST_BYTES));
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
}

static void rx_tap_no_buffer(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    setup_rxq(emu, 0, RX_BUF_CAP);
    rx_tap(emu, mac_ucast, 64);
    snprintf(got, len,
             "rx_ucast %" PRIu64 ", rx_ucast_drop %" PRIu64
             ", rx_queue_empty %" PRIu64,
             counter(emu, IONIC_LIF_RX_UCAST_PACKETS),
             counter(emu, IONIC_LIF_RX_UCAST_DROP_PACKETS),
             counter(emu, IONIC_LIF_RX_QUEUE_EMPTY));
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
}

static void rx_tap_too_big(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    setup_rxq(emu, 1, 64);
    rx_tap(emu, mac_ucast, 100);
    snprintf(got, len,
             "rx_ucast_drop %" PRIu64 "/%" PRIu64
             ", rx_desc_data_error %" PRIu64,
             counter(emu, IONIC_LIF_RX_UCAST_DROP_PACKETS),
             counter(emu, IONIC_LIF_RX_UCAST_DROP_BYTES),
             counter(emu, IONIC_LIF_RX_DESC_DATA_ERROR));
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
}

static void inbox_no_buffer(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    setup_rxq(emu, 0, RX_BUF_CAP);
    ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_ucast, 64), 64);
    ionic_eth_emu_poll_rx(emu);
    snprintf(got, len,
             "rx_ucast_drop %" PRIu64 ", rx_queue_empty %" PRIu64
             ", still queued %u",
             counter(emu, IONIC_LIF_RX_UCAST_DROP_PACKETS),
             counter(emu, IONIC_LIF_RX_QUEUE_EMPTY), emu->inbox_count);
    ionic_eth_emu_destroy(emu);
}

static void inbox_too_big_then_ok(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    setup_rxq(emu, 2, 64);
    ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_ucast, 100), 100);
    ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_ucast, 60), 60);
    ionic_eth_emu_poll_rx(emu);
    snprintf(got, len,
             "rx_ucast %" PRIu64 ", rx_ucast_drop %" PRIu64
             ", rx_desc_data_error %" PRIu64 ", still queued %u",
             counter(emu, IONIC_LIF_RX_UCAST_PACKETS),
             counter(emu, IONIC_LIF_RX_UCAST_DROP_PACKETS),
             counter(emu, IONIC_LIF_RX_DESC_DATA_ERROR), emu->inbox_count);
    ionic_eth_emu_destroy(emu);
}

static void inbox_full(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    int rc = 0;

    for (unsigned int i = 0; i <= ETH_INBOX_MAX_FRAMES; i++)
        rc = ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_bcast, 64), 64);
    snprintf(got, len, "last rc %d, rx_bcast_drop %" PRIu64 "/%" PRIu64, rc,
             counter(emu, IONIC_LIF_RX_BCAST_DROP_PACKETS),
             counter(emu, IONIC_LIF_RX_BCAST_DROP_BYTES));
    ionic_eth_emu_destroy(emu);
}

static const struct publish_case datapath_cases[] = {
    {"positive", "Tx with no host backend (a sink)", "tx_ucast 1/100", tx_sink},
    {"positive", "Tx refused by the host backend (-EAGAIN)",
     "tx_ucast 0, tx_ucast_drop 1/100", tx_send_fails},
    {"positive", "Rx from the TAP into a posted buffer", "rx_ucast 1/64",
     rx_tap_ok},
    {"positive", "Rx from the TAP with no buffer posted",
     "rx_ucast 0, rx_ucast_drop 1, rx_queue_empty 1", rx_tap_no_buffer},
    {"positive", "Rx from the TAP larger than the posted buffer",
     "rx_ucast_drop 1/100, rx_desc_data_error 1", rx_tap_too_big},
    {"negative", "Tx doorbell on a queue never initialized",
     "tx_queue_disabled 1", tx_disabled},
    {"negative", "Tx descriptor with length 0",
     "tx_desc_data_error 1, head 1 (completed anyway)", tx_len_zero},
    {"negative", "Tx descriptor pointing outside guest memory",
     "tx_dma_error 1, tx_ucast 0", tx_bad_addr},
    {"corner", "a mesh frame with no buffer posted",
     "rx_ucast_drop 0, rx_queue_empty 0, still queued 1 (back-pressure, not "
     "a drop)",
     inbox_no_buffer},
    {"corner", "a mesh frame too big for the buffer, then one that fits",
     "rx_ucast 1, rx_ucast_drop 1, rx_desc_data_error 1, still queued 0 (no "
     "head-of-line block)",
     inbox_too_big_then_ok},
    {"boundary", "one mesh frame past the inbox limit",
     "last rc -28, rx_bcast_drop 1/64", inbox_full},
};

/*
 * A row passes when @got equals the part of @expected before any " (": the
 * parenthesis is commentary for the reader.
 */
static void run_cases(const struct publish_case *cases, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const struct publish_case *c = &cases[i];
        char got[256] = "";
        const char *paren = strstr(c->expected, " (");
        size_t want_len =
            paren ? (size_t)(paren - c->expected) : strlen(c->expected);

        c->run(got, sizeof(got));
        if (strlen(got) == want_len &&
            strncmp(got, c->expected, want_len) == 0) {
            printf("ok %s: %s\n", c->category, c->description);
        } else {
            printf("FAIL %s: %s; expected: %s; got: %s\n", c->category,
                   c->description, c->expected, got);
            failures++;
        }
    }
}

/* ---- The --stats-file section ------------------------------------------ */

/* What ionic_eth_emu_write_stats() writes for @opaque, NUL-terminated. */
static char *stats_text(void *opaque, size_t *len)
{
    char *buf = NULL;
    FILE *fp = open_memstream(&buf, len);

    if (!fp)
        return NULL;
    ionic_eth_emu_write_stats(fp, opaque);
    fclose(fp);
    return buf;
}

/* The number from @p to the end of its line, or UINT64_MAX if it isn't one. */
static uint64_t line_u64(const char *p)
{
    uint64_t v;

    return ernic_parse_u64_span(p, strcspn(p, "\n"), &v) ? v : UINT64_MAX;
}

/* The value on the "  @name ... : N" line, or -1 as UINT64_MAX if absent. */
static uint64_t stats_value(const char *text, const char *name)
{
    size_t n = strlen(name);

    for (const char *l = text; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : l)
        if (!strncmp(l, "  ", 2) && !strncmp(l + 2, name, n) &&
            l[2 + n] == ' ') {
            const char *sep = strstr(l, " : ");

            return sep ? line_u64(sep + 3) : UINT64_MAX;
        }
    return UINT64_MAX;
}

static void file_traffic(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    char *text;
    size_t n;

    setup_txq(emu);
    tx_frame(emu, mac_ucast, 100);
    tx_frame(emu, mac_ucast, 200);
    text = stats_text(emu, &n);
    snprintf(got, len, "tx_ucast_packets %" PRIu64 ", tx_ucast_bytes %" PRIu64,
             stats_value(text, "tx_ucast_packets"),
             stats_value(text, "tx_ucast_bytes"));
    free(text);
    ionic_eth_emu_destroy(emu);
}

static void file_shape(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    unsigned int named = 0, lines = 0, misaligned = 0, nonzero = 0;
    char *text;
    size_t n;

    for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++)
        named += ionic_emu_lif_stat_name(q) != NULL;
    text = stats_text(emu, &n);
    for (const char *l = strchr(text, '\n') + 1; *l && *l != '\n';
         l = strchr(l, '\n') + 1) {
        lines++;
        /* Two spaces, a 35-wide name, then " : ". */
        if (strncmp(l + 2 + LIF_STAT_NAME_WIDTH, " : ", 3) != 0)
            misaligned++;
        else if (line_u64(l + 2 + LIF_STAT_NAME_WIDTH + 3) != 0)
            nonzero++;
    }
    snprintf(got, len,
             "header %d, lines == named %d, misaligned %u, nonzero %u, "
             "ends blank %d",
             !strncmp(text, "LIF Statistics:\n", 16), lines == named,
             misaligned, nonzero, n >= 2 && !strcmp(text + n - 2, "\n\n"));
    free(text);
    ionic_eth_emu_destroy(emu);
}

static void file_inbox_folded(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    char *text, *again;
    size_t n;

    /* The mesh thread's counter, folded by the writer. */
    ionic_emu_stats_rx_inbox_full(&emu->stats, make_frame(mac_ucast, 64), 64);
    text = stats_text(emu, &n);
    again = stats_text(emu, &n);
    snprintf(got, len,
             "rx_ucast_drop_packets %" PRIu64 ", rx_ucast_drop_bytes %" PRIu64
             ", second write %" PRIu64,
             stats_value(text, "rx_ucast_drop_packets"),
             stats_value(text, "rx_ucast_drop_bytes"),
             stats_value(again, "rx_ucast_drop_packets"));
    free(text);
    free(again);
    ionic_eth_emu_destroy(emu);
}

static void file_max_value(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    char *text;
    size_t n;

    emu->stats.lif[IONIC_LIF_RX_BCAST_BYTES] = UINT64_MAX;
    text = stats_text(emu, &n);
    snprintf(got, len, "rx_bcast_bytes %" PRIu64,
             stats_value(text, "rx_bcast_bytes"));
    free(text);
    ionic_eth_emu_destroy(emu);
}

static void file_no_emu(char *got, size_t len)
{
    size_t n = 1;
    char *text = stats_text(NULL, &n);

    snprintf(got, len, "bytes %zu", n);
    free(text);
}

static void file_longest_name(char *got, size_t len)
{
    size_t longest = 0;

    for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++) {
        const char *name = ionic_emu_lif_stat_name(q);

        if (name && strlen(name) > longest)
            longest = strlen(name);
    }
    snprintf(got, len, "longest %zu, width %d", longest, LIF_STAT_NAME_WIDTH);
}

static const struct publish_case stats_file_cases[] = {
    {"positive", "Tx 100 + 200 bytes unicast, then the stats file",
     "tx_ucast_packets 2, tx_ucast_bytes 300", file_traffic},
    {"positive", "the section's shape on an idle device",
     "header 1, lines == named 1, misaligned 0, nonzero 0, ends blank 1",
     file_shape},
    {"negative", "no eth emulator registered", "bytes 0", file_no_emu},
    {"boundary", "a counter at UINT64_MAX",
     "rx_bcast_bytes 18446744073709551615", file_max_value},
    {"boundary", "the name column fits the longest ionic_if.h name",
     "longest 35, width 35", file_longest_name},
    {"corner", "mesh inbox full off-thread, then the stats file twice",
     "rx_ucast_drop_packets 1, rx_ucast_drop_bytes 64, second write 1 (fold "
     "is not double-counted)",
     file_inbox_folded},
};

int main(void)
{
    address_boundaries();
    lif_status_address_boundaries();
    run_cases(publish_cases, sizeof(publish_cases) / sizeof(publish_cases[0]));
    run_cases(datapath_cases,
              sizeof(datapath_cases) / sizeof(datapath_cases[0]));
    run_cases(stats_file_cases,
              sizeof(stats_file_cases) / sizeof(stats_file_cases[0]));

    if (failures)
        printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
