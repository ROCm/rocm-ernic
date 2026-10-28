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

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "byteorder.h"
#include "ionic/ionic_bar.h"
#include "ionic/ionic_datapath.h"
#include "ionic/ionic_emu_stats.h"
#include "ionic/ionic_eth_emu.h"
#include "ionic_eth_emu_fakes.h"
#include "ionic/ionic_eth_emu_priv.h"
#include "ionic/ionic_fault.h"
#include "ionic/ionic_profile.h"
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
#define PORT_PA    0x2000u /* a struct ionic_port_info */

_Static_assert(LIF_PA + IONIC_LIF_INFO_STATS_OFF + IONIC_LIF_STATS_BYTES <=
                   TX_RING,
               "the first info block overlaps the Tx ring");
_Static_assert(RX_BUF + 2 * RX_BUF_CAP <= LIF_PA2,
               "the Rx buffers overlap the second info block");
_Static_assert(LIF_PA2 + IONIC_LIF_INFO_STATS_OFF + IONIC_LIF_STATS_BYTES <=
                   GUEST_MEM_SIZE,
               "the second info block is past the end of guest memory");

_Static_assert(PORT_PA >= LIF_PA2 + IONIC_LIF_INFO_STATS_OFF +
                              IONIC_LIF_STATS_BYTES &&
                   PORT_PA + IONIC_PORT_INFO_STATS_OFF +
                           IONIC_PORT_STATS_BYTES + 64 <=
                       GUEST_MEM_SIZE,
               "the port info block overlaps or runs past guest memory");

static int failures;

/* ---- Helpers ------------------------------------------------------------ */

static const uint8_t mac_ucast[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
static const uint8_t mac_bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static uint8_t g_frame[2048];

/* A frame to dst, filling all of g_frame: callers take what they need. */
static const uint8_t *make_frame(const uint8_t dst[6])
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
    g_net_sends = 0;
    g_net_rx_frame = NULL;
    g_net_rx_len = 0;
    memset(g_dp_rdma, 0, sizeof(g_dp_rdma));
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
    uint32_t doorbell = 1;

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = opcode;
    ernic_put_le64(cmd + 8, info_pa);
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
    return ernic_get_le64(g_mem + pa + IONIC_LIF_INFO_STATS_OFF +
                          (size_t)idx * 8);
}

/* Port counter @idx as the guest sees it at PORT_PA + 576. */
static uint64_t guest_port_qword(unsigned int idx)
{
    return ernic_get_le64(g_mem + PORT_PA + IONIC_PORT_INFO_STATS_OFF +
                          (size_t)idx * 8);
}

static void port_init(struct ionic_eth_emu *emu, uint64_t pa)
{
    devcmd(emu, IONIC_CMD_PORT_INIT, pa);
    /* PORT_INIT writes config and status; rows count from here. */
    g_nwrites = 0;
}

static uint64_t port_counter(struct ionic_eth_emu *emu, unsigned int idx)
{
    const struct ionic_emu_stats *st = ionic_eth_emu_get_stats(emu);

    if (!st)
        abort(); /* only a NULL emu has no stats */
    return st->port[idx];
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

    memset(d, 0, ETH_DESC_SIZE);
    ernic_put_le64(d, addr << TXQ_DESC_ADDR_SHIFT);
    ernic_put_le16(d + 8, len);
    tx_doorbell(emu, (uint16_t)(q->head + 1));
}

static void tx_frame(struct ionic_eth_emu *emu, const uint8_t dst[6],
                     uint16_t len)
{
    memcpy(g_mem + TX_BUF, make_frame(dst), len);
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

        memset(d, 0, ETH_DESC_SIZE);
        ernic_put_le16(d + 6, cap);
        ernic_put_le64(d + 8, RX_BUF + (uint64_t)i * RX_BUF_CAP);
    }
}

/* A frame arriving on the host TAP, then one receive poll. */
static void rx_tap(struct ionic_eth_emu *emu, const uint8_t dst[6], size_t len)
{
    static int fake_net;

    emu->net = (struct ionic_eth_net *)&fake_net;
    g_net_rx_frame = make_frame(dst);
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

/* ---- RDMA error counters, which the datapath keeps ---------------------- */

/* The datapath's names for its RDMA counters, in counter order. */
#define DP_RDMA_NAME(id, name) name,
static const char *const dp_rdma_names[IONIC_DP_RDMA_COUNT] = {
    IONIC_DP_RDMA_COUNTERS(DP_RDMA_NAME)};
#undef DP_RDMA_NAME

/*
 * The ionic_if.h name of each datapath counter's LIF slot is the
 * datapath's name with an rdma_ prefix, except for these.
 */
static const struct {
    const char *dp;
    const char *lif;
} dp_rdma_renamed[] = {
    {"resp_rx_out_of_atomic_res", "rdma_resp_rx_out_of_atomic_resource"},
    {"tx_cnp_pkts", "tx_rdma_cnp_packets"},
    {"rx_cnp_pkts", "rx_rdma_cnp_packets"},
    {"rx_ecn_pkts", "rx_rdma_ecn_packets"},
};

/*
 * The LIF slot datapath counter @i belongs in, or IONIC_LIF_STATS_QWORDS if
 * none is named for it.  Found by name, not through the emulator's own
 * table, so a counter copied into the wrong slot shows.
 */
static unsigned int lif_slot_of_dp(unsigned int i)
{
    char prefixed[64];
    const char *want = NULL;

    for (size_t r = 0; r < sizeof(dp_rdma_renamed) / sizeof(dp_rdma_renamed[0]);
         r++)
        if (strcmp(dp_rdma_names[i], dp_rdma_renamed[r].dp) == 0)
            want = dp_rdma_renamed[r].lif;
    if (!want) {
        int n =
            snprintf(prefixed, sizeof(prefixed), "rdma_%s", dp_rdma_names[i]);

        if (n < 0 || (size_t)n >= sizeof(prefixed))
            return IONIC_LIF_STATS_QWORDS;
        want = prefixed;
    }
    for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++) {
        const char *name = ionic_emu_lif_stat_name(q);

        if (name && strcmp(name, want) == 0)
            return q;
    }
    return IONIC_LIF_STATS_QWORDS;
}

/* A value for datapath counter @i that no other counter has. */
static uint64_t dp_rdma_value(unsigned int i)
{
    return UINT64_C(0x1000) + i;
}

/* The emulator only reads the datapath's counters when it has one; nothing
 * dereferences this stand-in. */
static void attach_fake_dp(struct ionic_eth_emu *emu)
{
    static int fake_dp;

    ionic_eth_emu_register_datapath(emu, (struct ionic_datapath *)&fake_dp);
}

static void rdma_counters_published(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    const struct ionic_emu_stats *st;
    unsigned int no_slot = 0, in_stats = 0, in_guest = 0;

    attach_fake_dp(emu);
    lif_init(emu, LIF_PA);
    for (unsigned int i = 0; i < IONIC_DP_RDMA_COUNT; i++)
        g_dp_rdma[i] = dp_rdma_value(i);
    ionic_eth_emu_publish_stats(emu, 5000);
    st = ionic_eth_emu_get_stats(emu);
    if (!st)
        abort(); /* only a NULL emu has no stats */
    for (unsigned int i = 0; i < IONIC_DP_RDMA_COUNT; i++) {
        unsigned int q = lif_slot_of_dp(i);

        if (q >= IONIC_LIF_STATS_QWORDS) {
            no_slot++;
            continue;
        }
        in_stats += st->lif[q] == dp_rdma_value(i);
        in_guest += guest_qword(LIF_PA, q) == dp_rdma_value(i);
    }
    snprintf(got, len, "counters %u, no slot %u, in stats %u, in guest %u",
             (unsigned int)IONIC_DP_RDMA_COUNT, no_slot, in_stats, in_guest);
    ionic_eth_emu_destroy(emu);
}

static void rdma_counters_device_reset(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(10);
    uint64_t before, cqe, cnp;

    attach_fake_dp(emu);
    lif_init(emu, LIF_PA);
    g_dp_rdma[IONIC_DP_REQ_RX_CQE_ERR] = 7;
    g_dp_rdma[IONIC_DP_TX_CNP_PKTS] = 9;
    before = counter(emu, IONIC_LIF_RDMA_REQ_RX_CQE_ERR);
    devcmd(emu, IONIC_CMD_RESET, 0);
    /* Read, and so refreshed from the datapath, after the reset. */
    cqe = counter(emu, IONIC_LIF_RDMA_REQ_RX_CQE_ERR);
    cnp = counter(emu, IONIC_LIF_TX_RDMA_CNP_PACKETS);
    lif_init(emu, LIF_PA);
    ionic_eth_emu_publish_stats(emu, 1000);
    snprintf(got, len,
             "before %" PRIu64 ", after %" PRIu64 "/%" PRIu64
             ", guest after re-init %" PRIu64 "/%" PRIu64,
             before, cqe, cnp,
             guest_qword(LIF_PA, IONIC_LIF_RDMA_REQ_RX_CQE_ERR),
             guest_qword(LIF_PA, IONIC_LIF_TX_RDMA_CNP_PACKETS));
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

/* ---- Port (MAC) stats block -------------------------------------------- */

#define PORT_BLOCK (PORT_PA + IONIC_PORT_INFO_STATS_OFF)
#define LIF_BLOCK  (LIF_PA + IONIC_LIF_INFO_STATS_OFF)

static void port_init_then_publish(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    port_init(emu, PORT_PA);
    memset(g_mem, POISON, sizeof(g_mem));
    ionic_eth_emu_publish_stats(emu, 5000);
    snprintf(got, len,
             "writes %u, block poisoned %zu, status poisoned %zu, sprom "
             "poisoned %zu",
             g_nwrites, poisoned(PORT_BLOCK, IONIC_PORT_STATS_BYTES),
             poisoned(PORT_PA, IONIC_PORT_INFO_STATS_OFF),
             poisoned(PORT_BLOCK + IONIC_PORT_STATS_BYTES, 64));
    ionic_eth_emu_destroy(emu);
}

static void port_traffic_then_publish(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    port_init(emu, PORT_PA);
    setup_txq(emu);
    setup_rxq(emu, 2, RX_BUF_CAP);
    tx_frame(emu, mac_ucast, 100);
    tx_frame(emu, mac_ucast, 200);
    rx_tap(emu, mac_bcast, 42);
    ionic_eth_emu_publish_stats(emu, 5000);
    snprintf(got, len,
             "frames_tx_ok %" PRIu64 ", octets_tx_ok %" PRIu64
             ", frames_rx_broadcast %" PRIu64 ", frames_rx_64b %" PRIu64,
             guest_port_qword(IONIC_PORT_FRAMES_TX_OK),
             guest_port_qword(IONIC_PORT_OCTETS_TX_OK),
             guest_port_qword(IONIC_PORT_FRAMES_RX_BROADCAST),
             guest_port_qword(IONIC_PORT_FRAMES_RX_64B));
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
}

/* LIF and port both initialised, then @opcode; which blocks are written? */
static void blocks_after(uint8_t opcode, char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(10);

    lif_init(emu, LIF_PA);
    port_init(emu, PORT_PA);
    devcmd(emu, opcode, 0);
    memset(g_mem, POISON, sizeof(g_mem));
    g_nwrites = 0;
    ionic_eth_emu_publish_stats(emu, 1000);
    snprintf(got, len, "writes %u, lif written %d, port written %d", g_nwrites,
             poisoned(LIF_BLOCK, IONIC_LIF_STATS_BYTES) == 0,
             poisoned(PORT_BLOCK, IONIC_PORT_STATS_BYTES) == 0);
    ionic_eth_emu_destroy(emu);
}

static void after_port_reset(char *got, size_t len)
{
    blocks_after(IONIC_CMD_PORT_RESET, got, len);
}

static void after_nop_port(char *got, size_t len)
{
    blocks_after(IONIC_CMD_NOP, got, len);
}

static void after_lif_reset_port(char *got, size_t len)
{
    blocks_after(IONIC_CMD_LIF_RESET, got, len);
}

static void after_device_reset_port(char *got, size_t len)
{
    blocks_after(IONIC_CMD_RESET, got, len);
}

static void before_port_init(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(10);

    lif_init(emu, LIF_PA);
    memset(g_mem, POISON, sizeof(g_mem));
    ionic_eth_emu_publish_stats(emu, 1000);
    snprintf(got, len, "writes %u, port block poisoned %zu", g_nwrites,
             poisoned(PORT_BLOCK, IONIC_PORT_STATS_BYTES));
    ionic_eth_emu_destroy(emu);
}

static void port_reset_then_reinit(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    lif_init(emu, LIF_PA);
    port_init(emu, PORT_PA);
    setup_rxq(emu, 2, RX_BUF_CAP);
    rx_tap(emu, mac_bcast, 42);
    ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_ucast), 64);
    devcmd(emu, IONIC_CMD_PORT_RESET, 0);
    port_init(emu, PORT_PA);
    memset(g_mem, POISON, sizeof(g_mem));
    ionic_eth_emu_publish_stats(emu, 5000);
    snprintf(got, len,
             "frames_rx_all %" PRIu64 ", frames_rx_broadcast %" PRIu64
             ", rx_bcast_packets %" PRIu64,
             guest_port_qword(IONIC_PORT_FRAMES_RX_ALL),
             guest_port_qword(IONIC_PORT_FRAMES_RX_BROADCAST),
             counter(emu, IONIC_LIF_RX_BCAST_PACKETS));
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
}

static const struct publish_case port_publish_cases[] = {
    {"positive", "PORT_INIT(pa), then publish",
     "writes 1, block poisoned 0, status poisoned 576, sprom poisoned 64 (only "
     "the 712 bytes at +576)",
     port_init_then_publish},
    {"positive", "Tx 100 + 200 bytes unicast, Rx a 42-byte broadcast, publish",
     "frames_tx_ok 2, octets_tx_ok 308, frames_rx_broadcast 1, frames_rx_64b 1",
     port_traffic_then_publish},
    {"positive", "LIF and port initialised, a NOP, then publish",
     "writes 2, lif written 1, port written 1", after_nop_port},
    {"negative", "LIF_RESET, then publish",
     "writes 1, lif written 0, port written 1 (+576 still, +320 no more)",
     after_lif_reset_port},
    {"negative", "LIF_INIT only, then publish",
     "writes 1, port block poisoned 712", before_port_init},
    {"negative", "PORT_RESET, then publish",
     "writes 1, lif written 1, port written 0 (+320 still, +576 no more)",
     after_port_reset},
    {"positive", "Rx traffic, PORT_RESET, PORT_INIT again, then publish",
     "frames_rx_all 0, frames_rx_broadcast 0, rx_bcast_packets 1 (the port "
     "restarts from zero; the LIF keeps its count)",
     port_reset_then_reinit},
    {"negative", "device reset, then publish",
     "writes 0, lif written 0, port written 0", after_device_reset_port},
};

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
    {"positive", "distinct datapath RDMA counters, then publish",
     "counters 30, no slot 0, in stats 30, in guest 30",
     rdma_counters_published},
    {"negative", "datapath RDMA counters, device reset, then read",
     "before 7, after 0/0, guest after re-init 0/0",
     rdma_counters_device_reset},
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
    ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_ucast), 64);
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
    ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_ucast), 100);
    ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_ucast), 60);
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
        rc = ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_bcast), 64);
    snprintf(got, len, "last rc %d, rx_bcast_drop %" PRIu64 "/%" PRIu64, rc,
             counter(emu, IONIC_LIF_RX_BCAST_DROP_PACKETS),
             counter(emu, IONIC_LIF_RX_BCAST_DROP_BYTES));
    ionic_eth_emu_destroy(emu);
}

/* A device whose port takes frames up to 1018 bytes (MTU 1000). */
static struct ionic_eth_emu *new_emu_mtu1000(void)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    emu->profile.link.mtu_max = 1000;
    return emu;
}

static void rx_tap_at(size_t frame_len, char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu_mtu1000();

    setup_rxq(emu, 1, RX_BUF_CAP);
    rx_tap(emu, mac_ucast, frame_len);
    snprintf(got, len,
             "rx_ucast %" PRIu64 ", rx_desc_data_error %" PRIu64
             ", frames_rx_too_long %" PRIu64 ", frames_rx_ok %" PRIu64,
             counter(emu, IONIC_LIF_RX_UCAST_PACKETS),
             counter(emu, IONIC_LIF_RX_DESC_DATA_ERROR),
             port_counter(emu, IONIC_PORT_FRAMES_RX_TOO_LONG),
             port_counter(emu, IONIC_PORT_FRAMES_RX_OK));
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
}

static void rx_tap_too_long(char *got, size_t len)
{
    rx_tap_at(1019, got, len);
}

static void rx_tap_max_frame(char *got, size_t len)
{
    rx_tap_at(1018, got, len);
}

static void rx_tap_no_buffer_port(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    setup_rxq(emu, 0, RX_BUF_CAP);
    rx_tap(emu, mac_ucast, 64);
    snprintf(got, len, "frames_rx_ok %" PRIu64 ", frames_rx_dropped %" PRIu64,
             port_counter(emu, IONIC_PORT_FRAMES_RX_OK),
             port_counter(emu, IONIC_PORT_FRAMES_RX_DROPPED));
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
}

/* A queued frame waits through two polls for a buffer, then is delivered. */
static void inbox_wait_port(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    uint64_t waiting;

    setup_rxq(emu, 0, RX_BUF_CAP);
    ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_ucast), 64);
    ionic_eth_emu_poll_rx(emu);
    ionic_eth_emu_poll_rx(emu);
    waiting = port_counter(emu, IONIC_PORT_FRAMES_RX_ALL);
    setup_rxq(emu, 1, RX_BUF_CAP);
    ionic_eth_emu_poll_rx(emu);
    snprintf(got, len,
             "frames_rx_all waiting %" PRIu64 ", after %" PRIu64
             ", frames_rx_dropped %" PRIu64 ", rx_ucast %" PRIu64,
             waiting, port_counter(emu, IONIC_PORT_FRAMES_RX_ALL),
             port_counter(emu, IONIC_PORT_FRAMES_RX_DROPPED),
             counter(emu, IONIC_LIF_RX_UCAST_PACKETS));
    ionic_eth_emu_destroy(emu);
}

static void inbox_too_long_port(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu_mtu1000();

    setup_rxq(emu, 2, RX_BUF_CAP);
    ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_ucast), 1019);
    ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_ucast), 60);
    ionic_eth_emu_poll_rx(emu);
    snprintf(got, len,
             "frames_rx_too_long %" PRIu64 ", rx_ucast %" PRIu64
             ", still queued %u",
             port_counter(emu, IONIC_PORT_FRAMES_RX_TOO_LONG),
             counter(emu, IONIC_LIF_RX_UCAST_PACKETS), emu->inbox_count);
    ionic_eth_emu_destroy(emu);
}

static void tx_send_fails_port(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    static int fake_net;

    emu->net = (struct ionic_eth_net *)&fake_net;
    g_net_send_rc = -EAGAIN;
    setup_txq(emu);
    tx_frame(emu, mac_ucast, 100);
    snprintf(got, len, "frames_tx_all %" PRIu64 ", tx_ucast_drop %" PRIu64,
             port_counter(emu, IONIC_PORT_FRAMES_TX_ALL),
             counter(emu, IONIC_LIF_TX_UCAST_DROP_PACKETS));
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
}

static void inbox_full_port(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);

    for (unsigned int i = 0; i <= ETH_INBOX_MAX_FRAMES; i++)
        ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_bcast), 64);
    snprintf(got, len,
             "frames_rx_all %" PRIu64 ", frames_rx_broadcast %" PRIu64
             ", frames_rx_65b_127b %" PRIu64 ", frames_rx_dropped %" PRIu64,
             port_counter(emu, IONIC_PORT_FRAMES_RX_ALL),
             port_counter(emu, IONIC_PORT_FRAMES_RX_BROADCAST),
             port_counter(emu, IONIC_PORT_FRAMES_RX_65B_127B),
             port_counter(emu, IONIC_PORT_FRAMES_RX_DROPPED));
    ionic_eth_emu_destroy(emu);
}

static const struct publish_case port_datapath_cases[] = {
    {"negative", "Rx from the TAP one byte over the port's maximum frame",
     "rx_ucast 0, rx_desc_data_error 0, frames_rx_too_long 1, frames_rx_ok 0 "
     "(the MAC drops it; the LIF never sees it)",
     rx_tap_too_long},
    {"boundary", "Rx from the TAP at exactly the maximum frame",
     "rx_ucast 1, rx_desc_data_error 0, frames_rx_too_long 0, frames_rx_ok 1",
     rx_tap_max_frame},
    {"positive", "Rx from the TAP with no buffer posted",
     "frames_rx_ok 1, frames_rx_dropped 1 (the MAC took it, the LIF dropped "
     "it)",
     rx_tap_no_buffer_port},
    {"corner",
     "a queued frame waits two polls for a buffer, then one is posted",
     "frames_rx_all waiting 1, after 1, frames_rx_dropped 0, rx_ucast 1 "
     "(counted once at the MAC)",
     inbox_wait_port},
    {"corner", "a queued frame over the maximum, then one that fits",
     "frames_rx_too_long 1, rx_ucast 1, still queued 0", inbox_too_long_port},
    {"positive", "the Rx inbox overflows by one broadcast",
     "frames_rx_all 1, frames_rx_broadcast 1, frames_rx_65b_127b 1, "
     "frames_rx_dropped 1 (the MAC took it in; the queued ones are counted "
     "when polled)",
     inbox_full_port},
    {"corner", "Tx refused by the host backend",
     "frames_tx_all 0, tx_ucast_drop 1 (it never reached the wire)",
     tx_send_fails_port},
};

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
    {"corner", "a queued frame with no buffer posted",
     "rx_ucast_drop 0, rx_queue_empty 0, still queued 1 (back-pressure, not "
     "a drop)",
     inbox_no_buffer},
    {"corner", "a queued frame too big for the buffer, then one that fits",
     "rx_ucast 1, rx_ucast_drop 1, rx_desc_data_error 1, still queued 0 (no "
     "head-of-line block)",
     inbox_too_big_then_ok},
    {"boundary", "one queued frame past the inbox limit",
     "last rc -28, rx_bcast_drop 1/64", inbox_full},
};

/* ---- Fault rows (eth_rx, eth_tx) ----------------------------------------- */

/* A fault set holding @spec, attached to @emu; free it after the emu. */
static struct ionic_fault_set *attach_fault(struct ionic_eth_emu *emu,
                                            const char *spec)
{
    struct ionic_fault_set *set = ionic_fault_set_new();
    char err[128];

    if (!set || ionic_fault_set_add(set, spec, err, sizeof(err)) != 0) {
        fprintf(stderr, "fault '%s': %s\n", spec, set ? err : "no memory");
        /* abort() rather than exit(), which isn't thread safe. */
        abort();
    }
    ionic_eth_emu_set_faults(emu, set);
    return set;
}

static const struct ionic_fault *site_fault(const struct ionic_fault_set *set,
                                            enum ionic_fault_site site)
{
    static const struct ionic_fault none;
    const struct ionic_fault *f = ionic_fault_set_get(set, site);

    return f ? f : &none;
}

static void fault_rx_drop(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    struct ionic_fault_set *set =
        attach_fault(emu, "eth_rx: every 1 effect drop");

    setup_rxq(emu, 1, RX_BUF_CAP);
    rx_tap(emu, mac_ucast, 64);
    snprintf(got, len,
             "rx_ucast %" PRIu64 ", rx_ucast_drop %" PRIu64
             ", rx_queue_empty %" PRIu64 ", head %u, fired %" PRIu64,
             counter(emu, IONIC_LIF_RX_UCAST_PACKETS),
             counter(emu, IONIC_LIF_RX_UCAST_DROP_PACKETS),
             counter(emu, IONIC_LIF_RX_QUEUE_EMPTY),
             (unsigned)emu->eth_q[IONIC_QTYPE_RXQ][0].head,
             site_fault(set, IONIC_FAULT_SITE_ETH_RX)->fired);
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
    ionic_fault_set_free(set);
}

static void fault_rx_nobuf(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    struct ionic_fault_set *set =
        attach_fault(emu, "eth_rx: every 1 effect nobuf");

    setup_rxq(emu, 1, RX_BUF_CAP);
    rx_tap(emu, mac_ucast, 64);
    snprintf(got, len,
             "rx_ucast %" PRIu64 ", rx_ucast_drop %" PRIu64 "/%" PRIu64
             ", rx_queue_empty %" PRIu64 ", head %u",
             counter(emu, IONIC_LIF_RX_UCAST_PACKETS),
             counter(emu, IONIC_LIF_RX_UCAST_DROP_PACKETS),
             counter(emu, IONIC_LIF_RX_UCAST_DROP_BYTES),
             counter(emu, IONIC_LIF_RX_QUEUE_EMPTY),
             (unsigned)emu->eth_q[IONIC_QTYPE_RXQ][0].head);
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
    ionic_fault_set_free(set);
}

static void fault_tx_drop(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    struct ionic_fault_set *set =
        attach_fault(emu, "eth_tx: every 1 effect drop");
    static int fake_net;

    setup_txq(emu);
    emu->net = (struct ionic_eth_net *)&fake_net;
    tx_frame(emu, mac_ucast, 100);
    snprintf(got, len,
             "tx_ucast %" PRIu64 ", tx_ucast_drop %" PRIu64 "/%" PRIu64
             ", head %u, sends %u",
             counter(emu, IONIC_LIF_TX_UCAST_PACKETS),
             counter(emu, IONIC_LIF_TX_UCAST_DROP_PACKETS),
             counter(emu, IONIC_LIF_TX_UCAST_DROP_BYTES),
             (unsigned)emu->eth_q[IONIC_QTYPE_TXQ][0].head, g_net_sends);
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
    ionic_fault_set_free(set);
}

static void fault_tx_every_2(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    struct ionic_fault_set *set =
        attach_fault(emu, "eth_tx: every 2 effect drop");

    setup_txq(emu);
    for (int i = 0; i < 4; i++)
        tx_frame(emu, mac_ucast, 100);
    snprintf(got, len,
             "tx_ucast %" PRIu64 "/%" PRIu64 ", tx_ucast_drop %" PRIu64
             "/%" PRIu64,
             counter(emu, IONIC_LIF_TX_UCAST_PACKETS),
             counter(emu, IONIC_LIF_TX_UCAST_BYTES),
             counter(emu, IONIC_LIF_TX_UCAST_DROP_PACKETS),
             counter(emu, IONIC_LIF_TX_UCAST_DROP_BYTES));
    ionic_eth_emu_destroy(emu);
    ionic_fault_set_free(set);
}

static void fault_rx_no_queue(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    struct ionic_fault_set *set =
        attach_fault(emu, "eth_rx: every 1 effect drop");

    rx_tap(emu, mac_ucast, 64);
    snprintf(got, len, "eligible %" PRIu64,
             site_fault(set, IONIC_FAULT_SITE_ETH_RX)->eligible);
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
    ionic_fault_set_free(set);
}

static void fault_tx_len_zero(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    struct ionic_fault_set *set =
        attach_fault(emu, "eth_tx: every 1 effect drop");

    setup_txq(emu);
    tx_one(emu, TX_BUF, 0);
    snprintf(got, len, "eligible %" PRIu64 ", tx_desc_data_error %" PRIu64,
             site_fault(set, IONIC_FAULT_SITE_ETH_TX)->eligible,
             counter(emu, IONIC_LIF_TX_DESC_DATA_ERROR));
    ionic_eth_emu_destroy(emu);
    ionic_fault_set_free(set);
}

static void fault_rx_after_2(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    struct ionic_fault_set *set =
        attach_fault(emu, "eth_rx: after 2 count 1 effect drop");

    setup_rxq(emu, 2, RX_BUF_CAP);
    for (int i = 0; i < 3; i++)
        rx_tap(emu, mac_ucast, 64);
    snprintf(got, len,
             "rx_ucast %" PRIu64 ", eligible %" PRIu64 ", fired %" PRIu64,
             counter(emu, IONIC_LIF_RX_UCAST_PACKETS),
             site_fault(set, IONIC_FAULT_SITE_ETH_RX)->eligible,
             site_fault(set, IONIC_FAULT_SITE_ETH_RX)->fired);
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
    ionic_fault_set_free(set);
}

static void fault_inbox_held(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    struct ionic_fault_set *set =
        attach_fault(emu, "eth_rx: after 2 effect drop");

    setup_rxq(emu, 0, RX_BUF_CAP);
    ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_ucast), 64);
    for (int i = 0; i < 3; i++)
        ionic_eth_emu_poll_rx(emu);
    snprintf(got, len,
             "eligible %" PRIu64 ", fired %" PRIu64 ", still queued %u",
             site_fault(set, IONIC_FAULT_SITE_ETH_RX)->eligible,
             site_fault(set, IONIC_FAULT_SITE_ETH_RX)->fired, emu->inbox_count);
    ionic_eth_emu_destroy(emu);
    ionic_fault_set_free(set);
}

static void fault_inbox_nobuf(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    struct ionic_fault_set *set =
        attach_fault(emu, "eth_rx: every 1 effect nobuf");

    setup_rxq(emu, 1, RX_BUF_CAP);
    ionic_eth_emu_queue_rx_frame(emu, make_frame(mac_ucast), 64);
    ionic_eth_emu_poll_rx(emu);
    snprintf(got, len,
             "rx_ucast_drop %" PRIu64 ", rx_queue_empty %" PRIu64
             ", still queued %u",
             counter(emu, IONIC_LIF_RX_UCAST_DROP_PACKETS),
             counter(emu, IONIC_LIF_RX_QUEUE_EMPTY), emu->inbox_count);
    ionic_eth_emu_destroy(emu);
    ionic_fault_set_free(set);
}

static void fault_other_site(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    struct ionic_fault_set *set =
        attach_fault(emu, "stats_vals: every 1 effect fail");

    setup_txq(emu);
    setup_rxq(emu, 1, RX_BUF_CAP);
    tx_frame(emu, mac_ucast, 100);
    rx_tap(emu, mac_ucast, 64);
    snprintf(got, len,
             "tx_ucast %" PRIu64 ", rx_ucast %" PRIu64
             ", stats_vals eligible %" PRIu64,
             counter(emu, IONIC_LIF_TX_UCAST_PACKETS),
             counter(emu, IONIC_LIF_RX_UCAST_PACKETS),
             site_fault(set, IONIC_FAULT_SITE_STATS_VALS)->eligible);
    emu->net = NULL;
    ionic_eth_emu_destroy(emu);
    ionic_fault_set_free(set);
}

static unsigned int g_filter_calls;

static bool count_filter(void *ctx, const void *frame, size_t len)
{
    (void)ctx;
    (void)frame;
    (void)len;
    g_filter_calls++;
    return true;
}

static void fault_tx_filter(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    struct ionic_fault_set *set =
        attach_fault(emu, "eth_tx: every 2 effect drop");

    g_filter_calls = 0;
    ionic_eth_emu_register_tx_filter(emu, count_filter, NULL);
    setup_txq(emu);
    tx_frame(emu, mac_ucast, 100);
    tx_frame(emu, mac_ucast, 100);
    snprintf(got, len, "filter calls %u, tx_ucast_drop %" PRIu64,
             g_filter_calls, counter(emu, IONIC_LIF_TX_UCAST_DROP_PACKETS));
    ionic_eth_emu_destroy(emu);
    ionic_fault_set_free(set);
}

static void tx_fault_preconditions(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        uint64_t address;
        uint16_t length;
        bool bad_ring;
        bool valid;
    } rows[] = {
        {"negative", "zero payload length",
         "no event before complete frame exists", TX_BUF, 0, false, true},
        {"negative", "unmapped payload", "no event on payload DMA failure",
         GUEST_MEM_SIZE, 64, false, true},
        {"boundary", "unmapped descriptor",
         "no event on descriptor DMA failure", TX_BUF, 64, true, true},
        {"corner", "uninitialized TX queue",
         "no event before queue registration", TX_BUF, 64, false, false},
    };
    for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
        struct ionic_eth_emu *emu = new_emu(1000);
        struct ionic_fault_set *set =
            attach_fault(emu, "eth_tx: after 1 effect drop");
        const struct ionic_fault *fault =
            site_fault(set, IONIC_FAULT_SITE_ETH_TX);
        struct eth_queue *q = &emu->eth_q[IONIC_QTYPE_TXQ][0];
        bool ok;

        setup_txq(emu);
        q->valid = rows[row].valid;
        if (rows[row].bad_ring)
            q->ring_base = GUEST_MEM_SIZE;
        tx_one(emu, rows[row].address, rows[row].length);
        ok = fault->eligible == 0 && fault->fired == 0;
        q->ring_base = TX_RING;
        q->valid = true;
        tx_frame(emu, mac_ucast, 64);
        ok = ok && fault->eligible == 1 && fault->fired == 1 &&
             counter(emu, IONIC_LIF_TX_UCAST_DROP_PACKETS) == 1;
        printf("%s %s: %s; expected: %s; next valid frame fires event 1\n",
               ok ? "ok" : "FAIL", rows[row].category, rows[row].description,
               rows[row].expected);
        if (!ok)
            failures++;
        ionic_eth_emu_destroy(emu);
        ionic_fault_set_free(set);
    }
}

static const struct publish_case fault_cases[] = {
    {"positive", "eth_rx every 1 drop, a buffer posted",
     "rx_ucast 0, rx_ucast_drop 0, rx_queue_empty 0, head 0, fired 1 (lost "
     "before the NIC: no counter)",
     fault_rx_drop},
    {"positive", "eth_rx every 1 nobuf, a buffer posted",
     "rx_ucast 0, rx_ucast_drop 1/64, rx_queue_empty 1, head 0 (as if the "
     "ring were empty)",
     fault_rx_nobuf},
    {"positive", "eth_tx every 1 drop, a host backend attached",
     "tx_ucast 0, tx_ucast_drop 1/100, head 1, sends 0 (completed, not sent)",
     fault_tx_drop},
    {"positive", "eth_tx every 2 drop over 4 frames",
     "tx_ucast 2/200, tx_ucast_drop 2/200", fault_tx_every_2},
    {"negative", "eth_rx fault with no Rx queue enabled",
     "eligible 0 (no frame is read)", fault_rx_no_queue},
    {"negative", "eth_tx fault on a descriptor of length 0",
     "eligible 0, tx_desc_data_error 1 (only gathered frames count)",
     fault_tx_len_zero},
    {"boundary", "eth_rx after 2 count 1 over 3 TAP frames, 2 buffers",
     "rx_ucast 2, eligible 3, fired 1", fault_rx_after_2},
    {"corner", "a queued frame held back for a buffer over 3 polls",
     "eligible 1, fired 0, still queued 1 (one event per frame)",
     fault_inbox_held},
    {"corner", "a queued frame under eth_rx every 1 nobuf",
     "rx_ucast_drop 1, rx_queue_empty 1, still queued 0", fault_inbox_nobuf},
    {"corner", "a set holding only a stats_vals fault",
     "tx_ucast 1, rx_ucast 1, stats_vals eligible 0 (the datapath is "
     "untouched)",
     fault_other_site},
    {"corner", "eth_tx every 2 drop with an in-process Tx filter",
     "filter calls 1, tx_ucast_drop 1 (a dropped frame is not offered)",
     fault_tx_filter},
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

/* The line after @l's, or NULL if @l's is the last. */
static const char *next_line(const char *l)
{
    const char *nl = strchr(l, '\n');

    return nl ? nl + 1 : NULL;
}

/* The value on the "  @name ... : N" line, or -1 as UINT64_MAX if absent. */
static uint64_t stats_value(const char *text, const char *name)
{
    size_t n = strlen(name);

    for (const char *l = text; l && *l; l = next_line(l))
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
    if (!text) {
        snprintf(got, len, "no stats text");
        ionic_eth_emu_destroy(emu);
        return;
    }
    for (const char *l = next_line(text); l && *l && *l != '\n';
         l = next_line(l)) {
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

static void file_inbox_full(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    char *text, *again;
    size_t n;

    /* A frame the inbox had no room for. */
    ionic_emu_stats_rx_inbox_full(&emu->stats, make_frame(mac_ucast), 64, 0);
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

static void file_port_section(char *got, size_t len)
{
    struct ionic_eth_emu *emu = new_emu(1000);
    unsigned int lines = 0, misaligned = 0;
    const char *sec;
    char *text;
    size_t n;

    setup_txq(emu);
    tx_frame(emu, mac_ucast, 100);
    text = stats_text(emu, &n);
    sec = strstr(text, "\nPort Statistics:\n");
    if (sec) {
        for (const char *l = sec + strlen("\nPort Statistics:\n");
             *l && *l != '\n'; l = strchr(l, '\n') + 1) {
            lines++;
            if (strncmp(l + 2 + PORT_STAT_NAME_WIDTH, " : ", 3) != 0)
                misaligned++;
        }
    }
    snprintf(got, len,
             "header %d, lines %u, misaligned %u, frames_tx_ok %" PRIu64
             ", octets_tx_ok %" PRIu64,
             sec != NULL, lines, misaligned, stats_value(text, "frames_tx_ok"),
             stats_value(text, "octets_tx_ok"));
    free(text);
    ionic_eth_emu_destroy(emu);
}

static void file_port_longest_name(char *got, size_t len)
{
    size_t longest = 0;

    for (unsigned int q = 0; q < IONIC_PORT_STATS_QWORDS; q++)
        if (strlen(ionic_emu_port_stat_name(q)) > longest)
            longest = strlen(ionic_emu_port_stat_name(q));
    snprintf(got, len, "longest %zu, width %d", longest, PORT_STAT_NAME_WIDTH);
}

static const struct publish_case stats_file_cases[] = {
    {"positive", "the port section after Tx 100 bytes",
     "header 1, lines 89, misaligned 0, frames_tx_ok 1, octets_tx_ok 104",
     file_port_section},
    {"boundary", "the port name column fits the longest port counter name",
     "longest 23, width 23", file_port_longest_name},
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
    {"corner", "Rx inbox full, then the stats file twice",
     "rx_ucast_drop_packets 1, rx_ucast_drop_bytes 64, second write 1 (writing "
     "counts nothing)",
     file_inbox_full},
};

int main(void)
{
    tx_fault_preconditions();
    address_boundaries();
    lif_status_address_boundaries();
    run_cases(publish_cases, sizeof(publish_cases) / sizeof(publish_cases[0]));
    run_cases(datapath_cases,
              sizeof(datapath_cases) / sizeof(datapath_cases[0]));
    run_cases(fault_cases, sizeof(fault_cases) / sizeof(fault_cases[0]));
    run_cases(port_publish_cases,
              sizeof(port_publish_cases) / sizeof(port_publish_cases[0]));
    run_cases(port_datapath_cases,
              sizeof(port_datapath_cases) / sizeof(port_datapath_cases[0]));
    run_cases(stats_file_cases,
              sizeof(stats_file_cases) / sizeof(stats_file_cases[0]));

    if (failures)
        printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
