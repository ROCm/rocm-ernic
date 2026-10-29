/*
 * Unit tests for the emulated ionic NIC's LIF counter block.
 *
 * ionic_get_stats64() in the guest reads struct ionic_lif_stats straight
 * out of DMA memory, so a counter in the wrong qword shows up as the wrong
 * `ip -s link` field, silently.  These rows pin the classification, the
 * drop accounting, the little-endian rendering and, against a copy of the
 * kernel's struct, every named index.
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
#include <stdio.h>
#include <string.h>

#include "byteorder.h"
#include "ionic/ionic_emu_stats.h"

static int failures;

static void check_u64(const char *category, const char *description,
                      const char *expected, uint64_t want, uint64_t got)
{
    if (want == got) {
        printf("ok %s: %s\n", category, description);
        return;
    }
    printf("FAIL %s: %s; expected: %s (%" PRIu64 "); got: %" PRIu64 "\n",
           category, description, expected, want, got);
    failures++;
}

/* ---- Frames ------------------------------------------------------------ */

static uint8_t frame_buf[9216];

/* A @len-byte frame to @dst (6 bytes), or to no MAC at all if @dst is NULL. */
static const uint8_t *frame_to(const uint8_t *dst, size_t len)
{
    memset(frame_buf, 0x5a, sizeof(frame_buf));
    if (dst && len >= 6)
        memcpy(frame_buf, dst, 6);
    return frame_buf;
}

static const uint8_t mac_ucast[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t mac_ucast0[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t mac_mcast[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01};
static const uint8_t mac_bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
/* Group bit set but not all ones: multicast, not broadcast. */
static const uint8_t mac_almost_bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xfe};

/* ---- Counting rows ------------------------------------------------------ */

enum op {
    OP_FRAME,
    OP_DROP,
    OP_INBOX_FULL,
    OP_RDMA,
};

/* One counter the row checks, and the value it must hold. */
struct expect_counter {
    int idx; /* enum ionic_lif_stat, or -1 to end the list */
    uint64_t value;
};

#define MAX_EXPECT 6

struct count_case {
    const char *category;
    const char *description;
    const char *expected;
    struct {
        enum op op;
        const uint8_t *dst; /* NULL: frame with no MAC, or no frame for drops */
        size_t len;
        bool is_tx;
        enum ionic_emu_drop why;
        unsigned int repeat;
    } in;
    /* Every listed counter must hold its value; every other one must be 0. */
    struct expect_counter want[MAX_EXPECT];
};

/* Terminates a want[] list. */
#define END_VALUES -1, 0

static const struct count_case count_cases[] = {
    {"positive",
     "tx 100-byte unicast (dst 02:...)",
     "tx_ucast_packets 1, tx_ucast_bytes 100, others 0",
     {OP_FRAME, mac_ucast, 100, true, 0, 1},
     {{IONIC_LIF_TX_UCAST_PACKETS, 1},
      {IONIC_LIF_TX_UCAST_BYTES, 100},
      {END_VALUES}}},
    {"positive",
     "tx multicast (dst 01:00:5e:...)",
     "only tx_mcast_* move",
     {OP_FRAME, mac_mcast, 200, true, 0, 1},
     {{IONIC_LIF_TX_MCAST_PACKETS, 1},
      {IONIC_LIF_TX_MCAST_BYTES, 200},
      {END_VALUES}}},
    {"positive",
     "tx broadcast (ff:ff:ff:ff:ff:ff)",
     "only tx_bcast_* move, not mcast",
     {OP_FRAME, mac_bcast, 300, true, 0, 1},
     {{IONIC_LIF_TX_BCAST_PACKETS, 1},
      {IONIC_LIF_TX_BCAST_BYTES, 300},
      {END_VALUES}}},
    {"positive",
     "rx unicast",
     "only rx_ucast_* move",
     {OP_FRAME, mac_ucast, 64, false, 0, 1},
     {{IONIC_LIF_RX_UCAST_PACKETS, 1},
      {IONIC_LIF_RX_UCAST_BYTES, 64},
      {END_VALUES}}},
    {"positive",
     "rx multicast",
     "only rx_mcast_* move",
     {OP_FRAME, mac_mcast, 64, false, 0, 1},
     {{IONIC_LIF_RX_MCAST_PACKETS, 1},
      {IONIC_LIF_RX_MCAST_BYTES, 64},
      {END_VALUES}}},
    {"positive",
     "rx broadcast",
     "only rx_bcast_* move",
     {OP_FRAME, mac_bcast, 64, false, 0, 1},
     {{IONIC_LIF_RX_BCAST_PACKETS, 1},
      {IONIC_LIF_RX_BCAST_BYTES, 64},
      {END_VALUES}}},
    {"positive",
     "rx drop: no buffer posted (-ENOBUFS)",
     "rx_ucast_drop_* +1 and rx_queue_empty +1; rx_ucast_packets unchanged",
     {OP_DROP, mac_ucast, 100, false, IONIC_EMU_DROP_NO_BUFFER, 1},
     {{IONIC_LIF_RX_UCAST_DROP_PACKETS, 1},
      {IONIC_LIF_RX_UCAST_DROP_BYTES, 100},
      {IONIC_LIF_RX_QUEUE_EMPTY, 1},
      {END_VALUES}}},
    {"positive",
     "rx drop: frame larger than the buffer (-EMSGSIZE)",
     "rx_desc_data_error +1 plus the drop counters",
     {OP_DROP, mac_mcast, 2000, false, IONIC_EMU_DROP_TOO_BIG, 1},
     {{IONIC_LIF_RX_MCAST_DROP_PACKETS, 1},
      {IONIC_LIF_RX_MCAST_DROP_BYTES, 2000},
      {IONIC_LIF_RX_DESC_DATA_ERROR, 1},
      {END_VALUES}}},
    {"positive",
     "rx drop: DMA write failed (-EIO)",
     "rx_dma_error +1 plus the drop counters",
     {OP_DROP, mac_bcast, 60, false, IONIC_EMU_DROP_DMA, 1},
     {{IONIC_LIF_RX_BCAST_DROP_PACKETS, 1},
      {IONIC_LIF_RX_BCAST_DROP_BYTES, 60},
      {IONIC_LIF_RX_DMA_ERROR, 1},
      {END_VALUES}}},
    {"positive",
     "rx drop: descriptor read failed",
     "rx_desc_fetch_error +1 plus the drop counters",
     {OP_DROP, mac_ucast, 60, false, IONIC_EMU_DROP_DESC_FETCH, 1},
     {{IONIC_LIF_RX_UCAST_DROP_PACKETS, 1},
      {IONIC_LIF_RX_UCAST_DROP_BYTES, 60},
      {IONIC_LIF_RX_DESC_FETCH_ERROR, 1},
      {END_VALUES}}},
    {"positive",
     "tx drop: the host backend refused the frame",
     "tx_ucast_drop_* +1 only",
     {OP_DROP, mac_ucast, 100, true, IONIC_EMU_DROP_SEND, 1},
     {{IONIC_LIF_TX_UCAST_DROP_PACKETS, 1},
      {IONIC_LIF_TX_UCAST_DROP_BYTES, 100},
      {END_VALUES}}},
    {"positive",
     "tx drop: an injected eth_tx fault",
     "tx_bcast_drop_* +1 only; no queue counter",
     {OP_DROP, mac_bcast, 64, true, IONIC_EMU_DROP_INJECTED, 1},
     {{IONIC_LIF_TX_BCAST_DROP_PACKETS, 1},
      {IONIC_LIF_TX_BCAST_DROP_BYTES, 64},
      {END_VALUES}}},
    {"positive",
     "tx drop: payload DMA read failed, frame unread",
     "tx_dma_error +1 only; no class counter (no MAC to classify)",
     {OP_DROP, NULL, 0, true, IONIC_EMU_DROP_DMA, 1},
     {{IONIC_LIF_TX_DMA_ERROR, 1}, {END_VALUES}}},
    {"positive",
     "tx drop: descriptor read failed",
     "tx_desc_fetch_error +1 only",
     {OP_DROP, NULL, 0, true, IONIC_EMU_DROP_DESC_FETCH, 1},
     {{IONIC_LIF_TX_DESC_FETCH_ERROR, 1}, {END_VALUES}}},
    {"positive",
     "tx drop: descriptor length or SG count invalid",
     "tx_desc_data_error +1 only",
     {OP_DROP, NULL, 0, true, IONIC_EMU_DROP_DESC_DATA, 1},
     {{IONIC_LIF_TX_DESC_DATA_ERROR, 1}, {END_VALUES}}},
    {"positive",
     "tx doorbell on a queue that is not initialized",
     "tx_queue_disabled +1 only",
     {OP_DROP, NULL, 0, true, IONIC_EMU_DROP_DISABLED, 1},
     {{IONIC_LIF_TX_QUEUE_DISABLED, 1}, {END_VALUES}}},
    {"positive",
     "Rx inbox full",
     "rx_ucast_drop_* +1; no queue counter (the guest never saw it)",
     {OP_INBOX_FULL, mac_ucast, 128, false, 0, 1},
     {{IONIC_LIF_RX_UCAST_DROP_PACKETS, 1},
      {IONIC_LIF_RX_UCAST_DROP_BYTES, 128},
      {END_VALUES}}},
    {"positive",
     "RDMA message sent, 4096 bytes",
     "tx_rdma_ucast_packets 1, tx_rdma_ucast_bytes 4096",
     {OP_RDMA, NULL, 4096, true, 0, 1},
     {{IONIC_LIF_TX_RDMA_UCAST_PACKETS, 1},
      {IONIC_LIF_TX_RDMA_UCAST_BYTES, 4096},
      {END_VALUES}}},
    {"positive",
     "RDMA message received, 3 times 10 bytes",
     "rx_rdma_ucast_packets 3, rx_rdma_ucast_bytes 30",
     {OP_RDMA, NULL, 10, false, 0, 3},
     {{IONIC_LIF_RX_RDMA_UCAST_PACKETS, 3},
      {IONIC_LIF_RX_RDMA_UCAST_BYTES, 30},
      {END_VALUES}}},
    {"negative",
     "zero-length frame",
     "no counter moves",
     {OP_FRAME, mac_ucast, 0, false, 0, 1},
     {{END_VALUES}}},
    {"negative",
     "rx frame shorter than 6 bytes (no destination MAC)",
     "no class counter moves; rx_desc_data_error +1",
     {OP_FRAME, NULL, 5, false, 0, 1},
     {{IONIC_LIF_RX_DESC_DATA_ERROR, 1}, {END_VALUES}}},
    {"negative",
     "tx frame shorter than 6 bytes",
     "no class counter moves; tx_desc_data_error +1",
     {OP_FRAME, NULL, 5, true, 0, 1},
     {{IONIC_LIF_TX_DESC_DATA_ERROR, 1}, {END_VALUES}}},
    {"negative",
     "zero-byte RDMA message",
     "no counter moves",
     {OP_RDMA, NULL, 0, true, 0, 1},
     {{END_VALUES}}},
    {"boundary",
     "60-byte frame",
     "counted, 60 bytes",
     {OP_FRAME, mac_ucast, 60, false, 0, 1},
     {{IONIC_LIF_RX_UCAST_PACKETS, 1},
      {IONIC_LIF_RX_UCAST_BYTES, 60},
      {END_VALUES}}},
    {"boundary",
     "9216-byte frame",
     "counted, 9216 bytes",
     {OP_FRAME, mac_ucast, 9216, true, 0, 1},
     {{IONIC_LIF_TX_UCAST_PACKETS, 1},
      {IONIC_LIF_TX_UCAST_BYTES, 9216},
      {END_VALUES}}},
    {"boundary",
     "6-byte frame: just the destination MAC",
     "classified, 6 bytes",
     {OP_FRAME, mac_bcast, 6, false, 0, 1},
     {{IONIC_LIF_RX_BCAST_PACKETS, 1},
      {IONIC_LIF_RX_BCAST_BYTES, 6},
      {END_VALUES}}},
    {"boundary",
     "dst 00:... has the group bit clear",
     "unicast",
     {OP_FRAME, mac_ucast0, 64, false, 0, 1},
     {{IONIC_LIF_RX_UCAST_PACKETS, 1},
      {IONIC_LIF_RX_UCAST_BYTES, 64},
      {END_VALUES}}},
    {"boundary",
     "dst ff:ff:ff:ff:ff:fe: group bit, not all ones",
     "multicast, not broadcast",
     {OP_FRAME, mac_almost_bcast, 64, false, 0, 1},
     {{IONIC_LIF_RX_MCAST_PACKETS, 1},
      {IONIC_LIF_RX_MCAST_BYTES, 64},
      {END_VALUES}}},
    {"corner",
     "inbox full 1000 times",
     "rx_bcast_drop_packets 1000, rx_bcast_drop_bytes 64000",
     {OP_INBOX_FULL, mac_bcast, 64, false, 0, 1000},
     {{IONIC_LIF_RX_BCAST_DROP_PACKETS, 1000},
      {IONIC_LIF_RX_BCAST_DROP_BYTES, 64000},
      {END_VALUES}}},
    {"corner",
     "inbox full with a frame too short to classify",
     "nothing counted (the guest could not have seen it either)",
     {OP_INBOX_FULL, NULL, 4, false, 0, 1},
     {{END_VALUES}}},
};

static void run_op(struct ionic_emu_stats *st, const struct count_case *c)
{
    const uint8_t *f =
        (c->in.dst || c->in.op == OP_FRAME || c->in.op == OP_INBOX_FULL)
            ? frame_to(c->in.dst, c->in.len)
            : NULL;

    switch (c->in.op) {
    case OP_FRAME:
        ionic_emu_stats_frame(st, f, c->in.len, c->in.is_tx);
        break;
    case OP_DROP:
        ionic_emu_stats_drop(st, f, c->in.len, c->in.is_tx, c->in.why);
        break;
    case OP_INBOX_FULL:
        ionic_emu_stats_rx_inbox_full(st, f, c->in.len, 0);
        break;
    case OP_RDMA:
        ionic_emu_stats_rdma(st, c->in.len, c->in.is_tx);
        break;
    default:
        break;
    }
}

static void test_counting(void)
{
    static struct ionic_emu_stats st;

    for (size_t i = 0; i < sizeof(count_cases) / sizeof(count_cases[0]); i++) {
        const struct count_case *c = &count_cases[i];
        uint64_t want[IONIC_LIF_STATS_QWORDS] = {0};
        char got[256] = "";
        size_t used = 0;
        bool ok = true;

        ionic_emu_stats_reset(&st);
        for (unsigned int r = 0; r < c->in.repeat; r++)
            run_op(&st, c);

        for (size_t k = 0; k < MAX_EXPECT && c->want[k].idx >= 0; k++)
            want[c->want[k].idx] = c->want[k].value;
        for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++) {
            if (st.lif[q] == want[q])
                continue;
            ok = false;
            /* One "q127=<u64>" entry at a time; stop when it won't fit. */
            char one[48];
            int n = snprintf(one, sizeof(one), "%sq%u=%" PRIu64,
                             used ? " " : "", q, st.lif[q]);

            if (n > 0 && used + (size_t)n < sizeof(got)) {
                memcpy(got + used, one, (size_t)n + 1);
                used += (size_t)n;
            }
        }
        if (ok) {
            printf("ok %s: %s\n", c->category, c->description);
        } else {
            printf("FAIL %s: %s; expected: %s; got: %s\n", c->category,
                   c->description, c->expected, got);
            failures++;
        }
    }
}

/* ---- Classification ---------------------------------------------------- */

struct class_case {
    const char *category;
    const char *description;
    const char *expected;
    const uint8_t *dst;
    size_t len;
    int want;
};

static const struct class_case class_cases[] = {
    {"positive", "unicast MAC", "IONIC_EMU_UCAST", mac_ucast, 64,
     IONIC_EMU_UCAST},
    {"positive", "IPv4 multicast MAC", "IONIC_EMU_MCAST", mac_mcast, 64,
     IONIC_EMU_MCAST},
    {"positive", "broadcast MAC", "IONIC_EMU_BCAST", mac_bcast, 64,
     IONIC_EMU_BCAST},
    {"negative", "NULL frame", "-1", NULL, 64, -1},
    {"boundary", "5 bytes", "-1: no full MAC", mac_ucast, 5, -1},
    {"boundary", "6 bytes", "classified", mac_bcast, 6, IONIC_EMU_BCAST},
};

static void test_classify(void)
{
    for (size_t i = 0; i < sizeof(class_cases) / sizeof(class_cases[0]); i++) {
        const struct class_case *c = &class_cases[i];
        const uint8_t *f = c->dst ? frame_to(c->dst, 64) : NULL;

        check_u64(c->category, c->description, c->expected, (uint64_t)c->want,
                  (uint64_t)ionic_emu_mac_class(f, c->len));
    }
}

/* ---- Fill, reset, wrap ------------------------------------------------ */

static uint64_t qword_at(const uint8_t *buf, unsigned int idx)
{
    return ernic_get_le64(buf + (size_t)idx * 8);
}

static void test_fill(void)
{
    static struct ionic_emu_stats st;
    uint8_t out[IONIC_LIF_STATS_BYTES];
    uint64_t sum;

    /* boundary: unsigned wrap, as a real le64 counter does */
    ionic_emu_stats_reset(&st);
    st.lif[IONIC_LIF_RX_UCAST_PACKETS] = UINT64_MAX - 1;
    ionic_emu_stats_frame(&st, frame_to(mac_ucast, 64), 64, false);
    check_u64("boundary", "counter at UINT64_MAX - 1, +1", "UINT64_MAX",
              UINT64_MAX, st.lif[IONIC_LIF_RX_UCAST_PACKETS]);
    ionic_emu_stats_frame(&st, frame_to(mac_ucast, 64), 64, false);
    check_u64("boundary", "counter at UINT64_MAX, +1", "wraps to 0", 0,
              st.lif[IONIC_LIF_RX_UCAST_PACKETS]);

    /* positive: fill() keeps the driver's sum rule */
    ionic_emu_stats_reset(&st);
    ionic_emu_stats_drop(&st, frame_to(mac_ucast, 64), 64, false,
                         IONIC_EMU_DROP_NO_BUFFER);
    ionic_emu_stats_drop(&st, frame_to(mac_mcast, 64), 64, false,
                         IONIC_EMU_DROP_TOO_BIG);
    ionic_emu_stats_rx_inbox_full(&st, frame_to(mac_bcast, 64), 64, 0);
    memset(out, 0xee, sizeof(out));
    ionic_emu_stats_fill(&st, out);
    sum = qword_at(out, IONIC_LIF_RX_UCAST_DROP_PACKETS) +
          qword_at(out, IONIC_LIF_RX_MCAST_DROP_PACKETS) +
          qword_at(out, IONIC_LIF_RX_BCAST_DROP_PACKETS);
    check_u64("positive",
              "fill(): the rx_*_drop_packets sum, as ionic_get_stats64() "
              "computes rx_dropped",
              "3 (two datapath drops and one inbox drop)", 3, sum);

    /* corner: every reserved qword after fill() is 0 */
    for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++)
        st.lif[q] = 0x1111111111111111ull * (q % 15 + 1);
    memset(out, 0xee, sizeof(out));
    ionic_emu_stats_fill(&st, out);
    {
        unsigned int bad = 0;

        for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++)
            if (!ionic_emu_lif_stat_name(q) && qword_at(out, q) != 0)
                bad++;
        check_u64("corner",
                  "every reserved qword after fill(), even with junk in the "
                  "block",
                  "0 nonzero reserved qwords", 0, bad);
    }

    /* corner: fill() writes little-endian */
    ionic_emu_stats_reset(&st);
    st.lif[IONIC_LIF_TX_UCAST_BYTES] = 0x0102030405060708ull;
    ionic_emu_stats_fill(&st, out);
    {
        static const uint8_t le[8] = {8, 7, 6, 5, 4, 3, 2, 1};

        check_u64("corner", "fill() writes little-endian",
                  "bytes 08 07 06 05 04 03 02 01 at qword 16", 0,
                  (uint64_t)memcmp(out + (size_t)IONIC_LIF_TX_UCAST_BYTES * 8,
                                   le, 8));
    }

    /* corner: reset() zeroes every qword */
    for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++)
        st.lif[q] = q + 1;
    ionic_emu_stats_rx_inbox_full(&st, frame_to(mac_ucast, 64), 64, 0);
    ionic_emu_stats_reset(&st);
    ionic_emu_stats_fill(&st, out);
    {
        uint64_t nonzero = 0;

        for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++)
            nonzero += qword_at(out, q) != 0;
        check_u64("corner", "reset(), then fill()",
                  "every qword 0, including the inbox drop", 0, nonzero);
    }
}

/* ---- Port (MAC) counters ------------------------------------------------ */

#define MAX_FRAME_1500 1518u /* MTU 1500 + VLAN_ETH_HLEN */
#define UNTAGGED       (-1)

/* A @len-byte frame to @dst, tagged with @tpid and PCP @pcp unless @pcp < 0. */
static const uint8_t *tagged_to(const uint8_t *dst, size_t len, uint16_t tpid,
                                int pcp)
{
    uint8_t *f = frame_buf; /* frame_to() fills it */

    (void)frame_to(dst, len);
    if (pcp >= 0 && len >= 16) {
        f[12] = (uint8_t)(tpid >> 8);
        f[13] = (uint8_t)tpid;
        f[14] = (uint8_t)(pcp << 5);
        f[15] = 0x01; /* VID 1 */
    }
    return f;
}

#define MAX_PORT_EXPECT 10

struct port_case {
    const char *category;
    const char *description;
    const char *expected;
    struct {
        const uint8_t *dst;
        size_t len;
        bool is_tx;
        uint16_t tpid;
        int pcp; /* UNTAGGED, or the PCP of a tag with @tpid */
        size_t max_frame;
    } in;
    bool accepted; /* what ionic_emu_port_frame() returns */
    /* Every listed counter must hold its value; every other one must be 0. */
    struct expect_counter want[MAX_PORT_EXPECT];
};

static const struct port_case port_cases[] = {
    {"positive",
     "rx 60-byte unicast (64 on the wire with FCS)",
     "frames_rx_ok/_all 1, octets_rx_ok/_all 64, frames_rx_64b, "
     "frames_rx_unicast and frames_rx_pri_0 1",
     {mac_ucast, 60, false, 0, UNTAGGED, MAX_FRAME_1500},
     true,
     {{IONIC_PORT_FRAMES_RX_OK, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_OK, 64},
      {IONIC_PORT_OCTETS_RX_ALL, 64},
      {IONIC_PORT_FRAMES_RX_64B, 1},
      {IONIC_PORT_FRAMES_RX_UNICAST, 1},
      {IONIC_PORT_FRAMES_RX_PRI_0, 1},
      {END_VALUES}}},
    {"positive",
     "rx multicast",
     "frames_rx_multicast 1, not unicast or broadcast",
     {mac_mcast, 100, false, 0, UNTAGGED, MAX_FRAME_1500},
     true,
     {{IONIC_PORT_FRAMES_RX_OK, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_OK, 104},
      {IONIC_PORT_OCTETS_RX_ALL, 104},
      {IONIC_PORT_FRAMES_RX_65B_127B, 1},
      {IONIC_PORT_FRAMES_RX_MULTICAST, 1},
      {IONIC_PORT_FRAMES_RX_PRI_0, 1},
      {END_VALUES}}},
    {"positive",
     "tx 1000-byte broadcast",
     "frames_tx_ok/_all 1, octets_tx_ok/_total 1004, frames_tx_512b_1023b, "
     "frames_tx_broadcast and frames_tx_pri_0 1",
     {mac_bcast, 1000, true, 0, UNTAGGED, 0},
     true,
     {{IONIC_PORT_FRAMES_TX_OK, 1},
      {IONIC_PORT_FRAMES_TX_ALL, 1},
      {IONIC_PORT_OCTETS_TX_OK, 1004},
      {IONIC_PORT_OCTETS_TX_TOTAL, 1004},
      {IONIC_PORT_FRAMES_TX_512B_1023B, 1},
      {IONIC_PORT_FRAMES_TX_BROADCAST, 1},
      {IONIC_PORT_FRAMES_TX_PRI_0, 1},
      {END_VALUES}}},
    {"positive",
     "rx 802.1Q frame with PCP 5",
     "frames_rx_vlan_good 1 and frames_rx_pri_5 1",
     {mac_ucast, 200, false, 0x8100, 5, MAX_FRAME_1500},
     true,
     {{IONIC_PORT_FRAMES_RX_OK, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_OK, 204},
      {IONIC_PORT_OCTETS_RX_ALL, 204},
      {IONIC_PORT_FRAMES_RX_128B_255B, 1},
      {IONIC_PORT_FRAMES_RX_UNICAST, 1},
      {IONIC_PORT_FRAMES_RX_VLAN_GOOD, 1},
      {IONIC_PORT_FRAMES_RX_PRI_5, 1},
      {END_VALUES}}},
    {"positive",
     "tx 802.1Q frame with PCP 3",
     "frames_tx_vlan 1 and frames_tx_pri_3 1",
     {mac_mcast, 300, true, 0x8100, 3, 0},
     true,
     {{IONIC_PORT_FRAMES_TX_OK, 1},
      {IONIC_PORT_FRAMES_TX_ALL, 1},
      {IONIC_PORT_OCTETS_TX_OK, 304},
      {IONIC_PORT_OCTETS_TX_TOTAL, 304},
      {IONIC_PORT_FRAMES_TX_256B_511B, 1},
      {IONIC_PORT_FRAMES_TX_MULTICAST, 1},
      {IONIC_PORT_FRAMES_TX_VLAN, 1},
      {IONIC_PORT_FRAMES_TX_PRI_3, 1},
      {END_VALUES}}},

    {"negative",
     "rx 1519 bytes on a port whose maximum frame is 1518",
     "too long: frames_rx_too_long, _oversized, _bad_all and _all 1, "
     "octets_rx_all 1523, frames_rx_1519b_2047b 1; frames_rx_ok 0 and the "
     "MAC drops it",
     {mac_ucast, 1519, false, 0, UNTAGGED, MAX_FRAME_1500},
     false,
     {{IONIC_PORT_FRAMES_RX_TOO_LONG, 1},
      {IONIC_PORT_FRAMES_RX_OVERSIZED, 1},
      {IONIC_PORT_FRAMES_RX_BAD_ALL, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_ALL, 1523},
      {IONIC_PORT_FRAMES_RX_1519B_2047B, 1},
      {END_VALUES}}},
    {"negative",
     "zero-length frame",
     "no counter moves",
     {mac_ucast, 0, false, 0, UNTAGGED, MAX_FRAME_1500},
     true,
     {{END_VALUES}}},
    {"negative",
     "tx 1600 bytes (no maximum applies to tx)",
     "sent and counted, frames_tx_1519b_2047b 1",
     {mac_ucast, 1600, true, 0, UNTAGGED, MAX_FRAME_1500},
     true,
     {{IONIC_PORT_FRAMES_TX_OK, 1},
      {IONIC_PORT_FRAMES_TX_ALL, 1},
      {IONIC_PORT_OCTETS_TX_OK, 1604},
      {IONIC_PORT_OCTETS_TX_TOTAL, 1604},
      {IONIC_PORT_FRAMES_TX_1519B_2047B, 1},
      {IONIC_PORT_FRAMES_TX_UNICAST, 1},
      {IONIC_PORT_FRAMES_TX_PRI_0, 1},
      {END_VALUES}}},

    {"boundary",
     "rx 59-byte frame",
     "padded to 60 as a sending MAC does: frames_rx_64b, octets 64, "
     "frames_rx_undersized 0",
     {mac_ucast, 59, false, 0, UNTAGGED, MAX_FRAME_1500},
     true,
     {{IONIC_PORT_FRAMES_RX_OK, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_OK, 64},
      {IONIC_PORT_OCTETS_RX_ALL, 64},
      {IONIC_PORT_FRAMES_RX_64B, 1},
      {IONIC_PORT_FRAMES_RX_UNICAST, 1},
      {IONIC_PORT_FRAMES_RX_PRI_0, 1},
      {END_VALUES}}},
    {"boundary",
     "rx 42-byte broadcast (an unpadded ARP from the TAP)",
     "counted as 64 on the wire, frames_rx_broadcast 1",
     {mac_bcast, 42, false, 0, UNTAGGED, MAX_FRAME_1500},
     true,
     {{IONIC_PORT_FRAMES_RX_OK, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_OK, 64},
      {IONIC_PORT_OCTETS_RX_ALL, 64},
      {IONIC_PORT_FRAMES_RX_64B, 1},
      {IONIC_PORT_FRAMES_RX_BROADCAST, 1},
      {IONIC_PORT_FRAMES_RX_PRI_0, 1},
      {END_VALUES}}},
    {"boundary",
     "rx exactly the maximum frame, 1518 bytes",
     "accepted: frames_rx_ok 1, frames_rx_1519b_2047b 1 (1522 with FCS)",
     {mac_ucast, 1518, false, 0, UNTAGGED, MAX_FRAME_1500},
     true,
     {{IONIC_PORT_FRAMES_RX_OK, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_OK, 1522},
      {IONIC_PORT_OCTETS_RX_ALL, 1522},
      {IONIC_PORT_FRAMES_RX_1519B_2047B, 1},
      {IONIC_PORT_FRAMES_RX_UNICAST, 1},
      {IONIC_PORT_FRAMES_RX_PRI_0, 1},
      {END_VALUES}}},
    {"boundary",
     "rx 802.1Q frame with PCP 7",
     "frames_rx_pri_7 1",
     {mac_ucast, 100, false, 0x8100, 7, MAX_FRAME_1500},
     true,
     {{IONIC_PORT_FRAMES_RX_OK, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_OK, 104},
      {IONIC_PORT_OCTETS_RX_ALL, 104},
      {IONIC_PORT_FRAMES_RX_65B_127B, 1},
      {IONIC_PORT_FRAMES_RX_UNICAST, 1},
      {IONIC_PORT_FRAMES_RX_VLAN_GOOD, 1},
      {IONIC_PORT_FRAMES_RX_PRI_7, 1},
      {END_VALUES}}},
    {"boundary",
     "rx 802.1Q frame with PCP 0",
     "frames_rx_pri_0 1 and frames_rx_vlan_good 1",
     {mac_ucast, 100, false, 0x8100, 0, MAX_FRAME_1500},
     true,
     {{IONIC_PORT_FRAMES_RX_OK, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_OK, 104},
      {IONIC_PORT_OCTETS_RX_ALL, 104},
      {IONIC_PORT_FRAMES_RX_65B_127B, 1},
      {IONIC_PORT_FRAMES_RX_UNICAST, 1},
      {IONIC_PORT_FRAMES_RX_VLAN_GOOD, 1},
      {IONIC_PORT_FRAMES_RX_PRI_0, 1},
      {END_VALUES}}},

    {"corner",
     "rx 802.1ad (QinQ) outer tag with PCP 6",
     "the outer tag's PCP: frames_rx_pri_6 1, frames_rx_vlan_good 1",
     {mac_ucast, 100, false, 0x88a8, 6, MAX_FRAME_1500},
     true,
     {{IONIC_PORT_FRAMES_RX_OK, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_OK, 104},
      {IONIC_PORT_OCTETS_RX_ALL, 104},
      {IONIC_PORT_FRAMES_RX_65B_127B, 1},
      {IONIC_PORT_FRAMES_RX_UNICAST, 1},
      {IONIC_PORT_FRAMES_RX_VLAN_GOOD, 1},
      {IONIC_PORT_FRAMES_RX_PRI_6, 1},
      {END_VALUES}}},
    {"corner",
     "rx 4-byte frame, too short for a destination MAC",
     "counted as a 64-byte frame of no class",
     {NULL, 4, false, 0, UNTAGGED, MAX_FRAME_1500},
     true,
     {{IONIC_PORT_FRAMES_RX_OK, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_OK, 64},
      {IONIC_PORT_OCTETS_RX_ALL, 64},
      {IONIC_PORT_FRAMES_RX_64B, 1},
      {IONIC_PORT_FRAMES_RX_PRI_0, 1},
      {END_VALUES}}},
    {"corner",
     "rx 9212 bytes (9216 on the wire) with no maximum (max_frame 0)",
     "accepted, frames_rx_other 1",
     {mac_ucast, 9212, false, 0, UNTAGGED, 0},
     true,
     {{IONIC_PORT_FRAMES_RX_OK, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_OK, 9216},
      {IONIC_PORT_OCTETS_RX_ALL, 9216},
      {IONIC_PORT_FRAMES_RX_OTHER, 1},
      {IONIC_PORT_FRAMES_RX_UNICAST, 1},
      {IONIC_PORT_FRAMES_RX_PRI_0, 1},
      {END_VALUES}}},
};

/* Compare every port qword with @want; describe the differences in @got. */
static bool port_matches(const struct ionic_emu_stats *st,
                         const struct expect_counter *want, size_t nwant,
                         char *got, size_t got_len)
{
    uint64_t v[IONIC_PORT_STATS_QWORDS] = {0};
    size_t used = 0;
    bool ok = true;

    for (size_t k = 0; k < nwant && want[k].idx >= 0; k++)
        v[want[k].idx] = want[k].value;
    got[0] = '\0';
    for (unsigned int q = 0; q < IONIC_PORT_STATS_QWORDS; q++) {
        char one[64];
        int n;

        if (st->port[q] == v[q])
            continue;
        ok = false;
        n = snprintf(one, sizeof(one), "%s%s=%" PRIu64, used ? " " : "",
                     ionic_emu_port_stat_name(q), st->port[q]);
        if (n > 0 && used + (size_t)n < got_len) {
            memcpy(got + used, one, (size_t)n + 1);
            used += (size_t)n;
        }
    }
    return ok;
}

static void test_port_counting(void)
{
    static struct ionic_emu_stats st;

    for (size_t i = 0; i < sizeof(port_cases) / sizeof(port_cases[0]); i++) {
        const struct port_case *c = &port_cases[i];
        const uint8_t *f =
            tagged_to(c->in.dst, c->in.len, c->in.tpid, c->in.pcp);
        char got[512];
        char returned[24] = "";
        bool accepted, ok;

        ionic_emu_stats_reset(&st);
        accepted = ionic_emu_port_frame(&st, f, c->in.len, c->in.is_tx,
                                        c->in.max_frame);
        ok = port_matches(&st, c->want, MAX_PORT_EXPECT, got, sizeof(got));
        if (accepted != c->accepted) {
            ok = false;
            snprintf(returned, sizeof(returned), " returned %s",
                     accepted ? "true" : "false");
        }
        if (ok) {
            printf("ok %s: %s\n", c->category, c->description);
        } else {
            printf("FAIL %s: %s; expected: %s; got: %s%s\n", c->category,
                   c->description, c->expected, got, returned);
            failures++;
        }
    }
}

/* Each size-bucket edge, as a wire length with FCS: the last length in a
 * bucket and the first of the next. */
struct bucket_edge {
    size_t wire;
    unsigned int rx_idx; /* the bucket @wire falls in */
};

static const struct bucket_edge bucket_edges[] = {
    {64, IONIC_PORT_FRAMES_RX_64B},
    {127, IONIC_PORT_FRAMES_RX_65B_127B},
    {255, IONIC_PORT_FRAMES_RX_128B_255B},
    {511, IONIC_PORT_FRAMES_RX_256B_511B},
    {1023, IONIC_PORT_FRAMES_RX_512B_1023B},
    {1518, IONIC_PORT_FRAMES_RX_1024B_1518B},
    {2047, IONIC_PORT_FRAMES_RX_1519B_2047B},
    {4095, IONIC_PORT_FRAMES_RX_2048B_4095B},
    {8191, IONIC_PORT_FRAMES_RX_4096B_8191B},
    {9215, IONIC_PORT_FRAMES_RX_8192B_9215B},
};

static void test_port_buckets(void)
{
    static struct ionic_emu_stats st;
    char desc[160], exp[160];

    for (size_t i = 0; i < sizeof(bucket_edges) / sizeof(bucket_edges[0]);
         i++) {
        const struct bucket_edge *e = &bucket_edges[i];

        for (unsigned int side = 0; side < 2; side++) {
            size_t wire = e->wire + side;
            unsigned int rx = e->rx_idx + side;
            unsigned int tx = rx - IONIC_PORT_FRAMES_RX_LESS_THAN_64B +
                              IONIC_PORT_FRAMES_TX_LESS_THAN_64B;
            size_t len = wire - IONIC_EMU_FCS_LEN;

            ionic_emu_stats_reset(&st);
            ionic_emu_port_frame(&st, frame_to(mac_ucast, len), len, false, 0);
            ionic_emu_port_frame(&st, frame_to(mac_ucast, len), len, true, 0);
            snprintf(desc, sizeof(desc),
                     "bucket edge: %zu bytes on the wire, rx and tx", wire);
            snprintf(exp, sizeof(exp), "%s and %s 1",
                     ionic_emu_port_stat_name(rx),
                     ionic_emu_port_stat_name(tx));
            check_u64("positive", desc, exp, 2, st.port[rx] + st.port[tx]);
        }
    }
}

/*
 * A frame the Rx inbox had no room for: the MAC took it in, so the port
 * counts it as it would any received frame, and the LIF dropped it.
 */
struct inbox_port_case {
    const char *category;
    const char *description;
    const char *expected;
    struct {
        const uint8_t *dst;
        size_t len;
        uint16_t tpid;
        int pcp; /* UNTAGGED, or the PCP of a tag with @tpid */
        size_t max_frame;
    } in;
    uint64_t lif_drops; /* rx_*_drop_packets, all classes */
    /* Every listed counter must hold its value; every other one must be 0. */
    struct expect_counter want[MAX_PORT_EXPECT];
};

static const struct inbox_port_case inbox_port_cases[] = {
    {"positive",
     "inbox full, 60-byte unicast",
     "frames_rx_all/_ok 1, octets_rx_all/_ok 64, frames_rx_64b, _unicast, "
     "_pri_0 and _dropped 1; one LIF drop",
     {mac_ucast, 60, 0, UNTAGGED, MAX_FRAME_1500},
     1,
     {{IONIC_PORT_FRAMES_RX_OK, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_OK, 64},
      {IONIC_PORT_OCTETS_RX_ALL, 64},
      {IONIC_PORT_FRAMES_RX_64B, 1},
      {IONIC_PORT_FRAMES_RX_UNICAST, 1},
      {IONIC_PORT_FRAMES_RX_PRI_0, 1},
      {IONIC_PORT_FRAMES_RX_DROPPED, 1},
      {END_VALUES}}},
    {"positive",
     "inbox full, 1000-byte broadcast tagged PCP 5",
     "frames_rx_broadcast, _vlan_good, _pri_5, _512b_1023b and _dropped 1; "
     "octets 1004",
     {mac_bcast, 1000, 0x8100, 5, MAX_FRAME_1500},
     1,
     {{IONIC_PORT_FRAMES_RX_OK, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_OK, 1004},
      {IONIC_PORT_OCTETS_RX_ALL, 1004},
      {IONIC_PORT_FRAMES_RX_512B_1023B, 1},
      {IONIC_PORT_FRAMES_RX_BROADCAST, 1},
      {IONIC_PORT_FRAMES_RX_VLAN_GOOD, 1},
      {IONIC_PORT_FRAMES_RX_PRI_5, 1},
      {IONIC_PORT_FRAMES_RX_DROPPED, 1},
      {END_VALUES}}},
    {"boundary",
     "inbox full, exactly the maximum frame",
     "frames_rx_ok 1 and _dropped 1, not too long; one LIF drop",
     {mac_ucast, MAX_FRAME_1500, 0, UNTAGGED, MAX_FRAME_1500},
     1,
     {{IONIC_PORT_FRAMES_RX_OK, 1},
      {IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_OK, MAX_FRAME_1500 + 4},
      {IONIC_PORT_OCTETS_RX_ALL, MAX_FRAME_1500 + 4},
      {IONIC_PORT_FRAMES_RX_1519B_2047B, 1},
      {IONIC_PORT_FRAMES_RX_UNICAST, 1},
      {IONIC_PORT_FRAMES_RX_PRI_0, 1},
      {IONIC_PORT_FRAMES_RX_DROPPED, 1},
      {END_VALUES}}},
    {"negative",
     "inbox full, one byte over the maximum frame",
     "frames_rx_too_long, _oversized and _bad_all 1, not _dropped; no LIF "
     "drop (the MAC refused it first)",
     {mac_ucast, MAX_FRAME_1500 + 1, 0, UNTAGGED, MAX_FRAME_1500},
     0,
     {{IONIC_PORT_FRAMES_RX_ALL, 1},
      {IONIC_PORT_OCTETS_RX_ALL, MAX_FRAME_1500 + 5},
      {IONIC_PORT_FRAMES_RX_1519B_2047B, 1},
      {IONIC_PORT_FRAMES_RX_TOO_LONG, 1},
      {IONIC_PORT_FRAMES_RX_OVERSIZED, 1},
      {IONIC_PORT_FRAMES_RX_BAD_ALL, 1},
      {END_VALUES}}},
    {"corner",
     "inbox full with a frame too short to classify",
     "nothing counted at the port or the LIF",
     {mac_ucast, 4, 0, UNTAGGED, MAX_FRAME_1500},
     0,
     {{END_VALUES}}},
};

static void test_port_inbox_full(void)
{
    static struct ionic_emu_stats st;

    for (size_t i = 0;
         i < sizeof(inbox_port_cases) / sizeof(inbox_port_cases[0]); i++) {
        const struct inbox_port_case *c = &inbox_port_cases[i];
        const uint8_t *f =
            tagged_to(c->in.dst, c->in.len, c->in.tpid, c->in.pcp);
        uint64_t drops;
        char got[512];
        char lif[48] = "";
        bool ok;

        ionic_emu_stats_reset(&st);
        ionic_emu_stats_rx_inbox_full(&st, f, c->in.len, c->in.max_frame);
        ok = port_matches(&st, c->want, MAX_PORT_EXPECT, got, sizeof(got));
        drops = st.lif[IONIC_LIF_RX_UCAST_DROP_PACKETS] +
                st.lif[IONIC_LIF_RX_MCAST_DROP_PACKETS] +
                st.lif[IONIC_LIF_RX_BCAST_DROP_PACKETS];
        if (drops != c->lif_drops) {
            ok = false;
            snprintf(lif, sizeof(lif), " lif drops %" PRIu64, drops);
        }
        if (ok) {
            printf("ok %s: %s\n", c->category, c->description);
        } else {
            printf("FAIL %s: %s; expected: %s; got: %s%s\n", c->category,
                   c->description, c->expected, got, lif);
            failures++;
        }
    }
}

/* PORT_RESET's ionic_emu_port_clear(): the port restarts, the LIF does not. */
static void test_port_clear(void)
{
    static const struct {
        const char *category;
        const char *description;
        const char *expected;
        unsigned int frames;      /* received and counted before the clear */
        unsigned int inbox_drops; /* dropped by a full inbox before it */
        unsigned int after;       /* received after the clear */
    } cases[] = {
        {"positive", "traffic, then clear", "port 0, LIF rx_ucast_packets 3", 3,
         0, 0},
        {"corner", "an inbox drop, then clear",
         "port 0, LIF rx_ucast_drop_packets 1 (kept)", 0, 1, 0},
        {"positive", "traffic, clear, then one more frame",
         "port frames_rx_all 1, LIF rx_ucast_packets 3", 2, 0, 1},
        {"boundary", "clear with nothing counted", "port 0, LIF 0", 0, 0, 0},
    };
    static struct ionic_emu_stats st;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const uint8_t *f = frame_to(mac_ucast, 60);
        uint64_t port_sum = 0;

        ionic_emu_stats_reset(&st);
        for (unsigned int n = 0; n < cases[i].frames; n++) {
            ionic_emu_port_frame(&st, f, 60, false, MAX_FRAME_1500);
            ionic_emu_stats_frame(&st, f, 60, false);
        }
        for (unsigned int n = 0; n < cases[i].inbox_drops; n++)
            ionic_emu_stats_rx_inbox_full(&st, f, 60, MAX_FRAME_1500);
        ionic_emu_port_clear(&st);
        for (unsigned int n = 0; n < cases[i].after; n++) {
            ionic_emu_port_frame(&st, f, 60, false, MAX_FRAME_1500);
            ionic_emu_stats_frame(&st, f, 60, false);
        }
        for (unsigned int q = 0; q < IONIC_PORT_STATS_QWORDS; q++)
            if (q != IONIC_PORT_FRAMES_RX_ALL)
                port_sum += cases[i].after ? 0 : st.port[q];
        check_u64(cases[i].category, cases[i].description, cases[i].expected,
                  cases[i].after, st.port[IONIC_PORT_FRAMES_RX_ALL]);
        check_u64(cases[i].category, cases[i].description,
                  "every other port counter 0", 0, port_sum);
        check_u64(cases[i].category, cases[i].description,
                  "LIF rx_ucast_packets kept",
                  (uint64_t)cases[i].frames + cases[i].after,
                  st.lif[IONIC_LIF_RX_UCAST_PACKETS]);
        check_u64(cases[i].category, cases[i].description,
                  "LIF rx_ucast_drop_packets kept", cases[i].inbox_drops,
                  st.lif[IONIC_LIF_RX_UCAST_DROP_PACKETS]);
    }
}

static void test_port_misc(void)
{
    static struct ionic_emu_stats st;
    uint8_t out[IONIC_PORT_STATS_BYTES];
    uint64_t sum = 0;

    /* corner: over a mixed run the rx size buckets sum to frames_rx_all */
    ionic_emu_stats_reset(&st);
    for (unsigned int i = 0; i < 1000; i++) {
        size_t len = 1 + (size_t)(i * 7919u) % (sizeof(frame_buf) - 1u);

        ionic_emu_port_frame(&st, frame_to(mac_ucast, len), len, false,
                             MAX_FRAME_1500);
    }
    for (unsigned int q = IONIC_PORT_FRAMES_RX_LESS_THAN_64B;
         q <= IONIC_PORT_FRAMES_RX_OTHER; q++)
        sum += st.port[q];
    check_u64("corner", "1,000 mixed rx frames, some too long",
              "the size buckets sum to frames_rx_all",
              st.port[IONIC_PORT_FRAMES_RX_ALL], sum);
    check_u64("corner", "the same run",
              "frames_rx_ok + frames_rx_bad_all "
              "= frames_rx_all",
              st.port[IONIC_PORT_FRAMES_RX_ALL],
              st.port[IONIC_PORT_FRAMES_RX_OK] +
                  st.port[IONIC_PORT_FRAMES_RX_BAD_ALL]);

    /* positive: a LIF drop of a frame the MAC took in */
    ionic_emu_stats_reset(&st);
    ionic_emu_port_rx_dropped(&st);
    check_u64("positive", "rx frame the LIF could not deliver",
              "frames_rx_dropped 1", 1, st.port[IONIC_PORT_FRAMES_RX_DROPPED]);

    /* positive: an Rx inbox overflow is a port rx drop too, once */
    ionic_emu_stats_reset(&st);
    ionic_emu_stats_rx_inbox_full(&st, frame_to(mac_ucast, 64), 64, 0);
    ionic_emu_port_fill(&st, out);
    ionic_emu_port_fill(&st, out);
    check_u64("positive", "Rx inbox full, then fill() twice",
              "frames_rx_dropped 1 in the block", 1,
              qword_at(out, IONIC_PORT_FRAMES_RX_DROPPED));

    /* corner: the inject-only counters stay 0 under normal traffic */
    ionic_emu_stats_reset(&st);
    for (unsigned int i = 0; i < 100; i++) {
        ionic_emu_port_frame(&st, frame_to(mac_ucast, 1000), 1000, false,
                             MAX_FRAME_1500);
        ionic_emu_port_frame(&st, frame_to(mac_ucast, 1000), 1000, true, 0);
    }
    {
        static const unsigned int inject_only[] = {
            IONIC_PORT_FRAMES_RX_BAD_FCS,    IONIC_PORT_FRAMES_RX_PAUSE,
            IONIC_PORT_FRAMES_RX_BAD_LENGTH, IONIC_PORT_FRAMES_RX_UNDERSIZED,
            IONIC_PORT_FRAMES_RX_FRAGMENTS,  IONIC_PORT_FRAMES_RX_JABBER,
            IONIC_PORT_FRAMES_RX_PRIPAUSE,   IONIC_PORT_FRAMES_RX_STOMPED_CRC,
            IONIC_PORT_FRAMES_TX_BAD,        IONIC_PORT_FRAMES_TX_PAUSE,
            IONIC_PORT_FRAMES_TX_PRIPAUSE,   IONIC_PORT_RX_PAUSE_1US_COUNT,
            IONIC_PORT_FRAMES_TX_TRUNCATED,
        };
        uint64_t nonzero = 0;

        for (size_t i = 0; i < sizeof(inject_only) / sizeof(inject_only[0]);
             i++)
            nonzero += st.port[inject_only[i]] != 0;
        for (unsigned int q = IONIC_PORT_TX_PRIPAUSE_0_1US_COUNT;
             q <= IONIC_PORT_RX_PRIPAUSE_7_1US_COUNT; q++)
            nonzero += st.port[q] != 0;
        check_u64("corner", "100 ordinary frames each way",
                  "every inject-only counter (FCS, jabber, pause, ...) 0", 0,
                  nonzero);
    }

    /* corner: fill() writes little-endian, and every one of the 89 qwords */
    ionic_emu_stats_reset(&st);
    for (unsigned int q = 0; q < IONIC_PORT_STATS_QWORDS; q++)
        st.port[q] = 0x0102030405060700ull + q;
    memset(out, 0xee, sizeof(out));
    ionic_emu_port_fill(&st, out);
    {
        uint64_t bad = 0;

        for (unsigned int q = 0; q < IONIC_PORT_STATS_QWORDS; q++)
            bad += qword_at(out, q) != 0x0102030405060700ull + q;
        check_u64("corner", "port fill() with a distinct value per qword",
                  "all 89 qwords, little-endian, in order", 0, bad);
    }

    /* corner: reset() zeroes the port block */
    ionic_emu_stats_reset(&st);
    ionic_emu_port_fill(&st, out);
    {
        uint64_t nonzero = 0;

        for (unsigned int q = 0; q < IONIC_PORT_STATS_QWORDS; q++)
            nonzero += qword_at(out, q) != 0;
        check_u64("corner", "reset(), then port fill()", "every qword 0", 0,
                  nonzero);
    }
}

/* ---- Layout pin --------------------------------------------------------- */

/*
 * struct ionic_lif_stats and struct ionic_lif_info, copied from the
 * kernel's drivers/net/ethernet/pensando/ionic/ionic_if.h (Linux 7.2) with
 * __le64 spelled uint64_t.  Keep the field order exactly as upstream.
 */
struct kernel_lif_stats {
    uint64_t rx_ucast_bytes, rx_ucast_packets, rx_mcast_bytes, rx_mcast_packets,
        rx_bcast_bytes, rx_bcast_packets, rsvd0, rsvd1;
    uint64_t rx_ucast_drop_bytes, rx_ucast_drop_packets, rx_mcast_drop_bytes,
        rx_mcast_drop_packets, rx_bcast_drop_bytes, rx_bcast_drop_packets,
        rx_dma_error, rsvd2;
    uint64_t tx_ucast_bytes, tx_ucast_packets, tx_mcast_bytes, tx_mcast_packets,
        tx_bcast_bytes, tx_bcast_packets, rsvd3, rsvd4;
    uint64_t tx_ucast_drop_bytes, tx_ucast_drop_packets, tx_mcast_drop_bytes,
        tx_mcast_drop_packets, tx_bcast_drop_bytes, tx_bcast_drop_packets,
        tx_dma_error, rsvd5;
    uint64_t rx_queue_disabled, rx_queue_empty, rx_queue_error,
        rx_desc_fetch_error, rx_desc_data_error, rsvd6, rsvd7, rsvd8;
    uint64_t tx_queue_disabled, tx_queue_error, tx_desc_fetch_error,
        tx_desc_data_error, tx_queue_empty, rsvd10, rsvd11, rsvd12;
    uint64_t tx_rdma_ucast_bytes, tx_rdma_ucast_packets, tx_rdma_mcast_bytes,
        tx_rdma_mcast_packets, tx_rdma_cnp_packets, rsvd13, rsvd14, rsvd15;
    uint64_t rx_rdma_ucast_bytes, rx_rdma_ucast_packets, rx_rdma_mcast_bytes,
        rx_rdma_mcast_packets, rx_rdma_cnp_packets, rx_rdma_ecn_packets, rsvd16,
        rsvd17;
    uint64_t rsvd18_49[32];
    uint64_t rdma_req_rx_pkt_seq_err, rdma_req_rx_rnr_retry_err,
        rdma_req_rx_remote_access_err, rdma_req_rx_remote_inv_req_err,
        rdma_req_rx_remote_oper_err, rdma_req_rx_implied_nak_seq_err,
        rdma_req_rx_cqe_err, rdma_req_rx_cqe_flush_err;
    uint64_t rdma_req_rx_dup_responses, rdma_req_rx_invalid_packets,
        rdma_req_tx_local_access_err, rdma_req_tx_local_oper_err,
        rdma_req_tx_memory_mgmt_err, rsvd52, rsvd53, rsvd54;
    uint64_t rdma_resp_rx_dup_requests, rdma_resp_rx_out_of_buffer,
        rdma_resp_rx_out_of_seq_pkts, rdma_resp_rx_cqe_err,
        rdma_resp_rx_cqe_flush_err, rdma_resp_rx_local_len_err,
        rdma_resp_rx_inv_request_err, rdma_resp_rx_local_qp_oper_err;
    uint64_t rdma_resp_rx_out_of_atomic_resource, rdma_resp_tx_pkt_seq_err,
        rdma_resp_tx_remote_inv_req_err, rdma_resp_tx_remote_access_err,
        rdma_resp_tx_remote_oper_err, rdma_resp_tx_rnr_retry_err, rsvd57,
        rsvd58;
};

struct kernel_lif_info {
    uint8_t config[256];
    uint8_t status[64];
    struct kernel_lif_stats stats;
};

/* Every named counter: kernel field, our index, and the name we print. */
#define LIF_FIELDS(X)                                                         \
    X(rx_ucast_bytes, IONIC_LIF_RX_UCAST_BYTES)                               \
    X(rx_ucast_packets, IONIC_LIF_RX_UCAST_PACKETS)                           \
    X(rx_mcast_bytes, IONIC_LIF_RX_MCAST_BYTES)                               \
    X(rx_mcast_packets, IONIC_LIF_RX_MCAST_PACKETS)                           \
    X(rx_bcast_bytes, IONIC_LIF_RX_BCAST_BYTES)                               \
    X(rx_bcast_packets, IONIC_LIF_RX_BCAST_PACKETS)                           \
    X(rx_ucast_drop_bytes, IONIC_LIF_RX_UCAST_DROP_BYTES)                     \
    X(rx_ucast_drop_packets, IONIC_LIF_RX_UCAST_DROP_PACKETS)                 \
    X(rx_mcast_drop_bytes, IONIC_LIF_RX_MCAST_DROP_BYTES)                     \
    X(rx_mcast_drop_packets, IONIC_LIF_RX_MCAST_DROP_PACKETS)                 \
    X(rx_bcast_drop_bytes, IONIC_LIF_RX_BCAST_DROP_BYTES)                     \
    X(rx_bcast_drop_packets, IONIC_LIF_RX_BCAST_DROP_PACKETS)                 \
    X(rx_dma_error, IONIC_LIF_RX_DMA_ERROR)                                   \
    X(tx_ucast_bytes, IONIC_LIF_TX_UCAST_BYTES)                               \
    X(tx_ucast_packets, IONIC_LIF_TX_UCAST_PACKETS)                           \
    X(tx_mcast_bytes, IONIC_LIF_TX_MCAST_BYTES)                               \
    X(tx_mcast_packets, IONIC_LIF_TX_MCAST_PACKETS)                           \
    X(tx_bcast_bytes, IONIC_LIF_TX_BCAST_BYTES)                               \
    X(tx_bcast_packets, IONIC_LIF_TX_BCAST_PACKETS)                           \
    X(tx_ucast_drop_bytes, IONIC_LIF_TX_UCAST_DROP_BYTES)                     \
    X(tx_ucast_drop_packets, IONIC_LIF_TX_UCAST_DROP_PACKETS)                 \
    X(tx_mcast_drop_bytes, IONIC_LIF_TX_MCAST_DROP_BYTES)                     \
    X(tx_mcast_drop_packets, IONIC_LIF_TX_MCAST_DROP_PACKETS)                 \
    X(tx_bcast_drop_bytes, IONIC_LIF_TX_BCAST_DROP_BYTES)                     \
    X(tx_bcast_drop_packets, IONIC_LIF_TX_BCAST_DROP_PACKETS)                 \
    X(tx_dma_error, IONIC_LIF_TX_DMA_ERROR)                                   \
    X(rx_queue_disabled, IONIC_LIF_RX_QUEUE_DISABLED)                         \
    X(rx_queue_empty, IONIC_LIF_RX_QUEUE_EMPTY)                               \
    X(rx_queue_error, IONIC_LIF_RX_QUEUE_ERROR)                               \
    X(rx_desc_fetch_error, IONIC_LIF_RX_DESC_FETCH_ERROR)                     \
    X(rx_desc_data_error, IONIC_LIF_RX_DESC_DATA_ERROR)                       \
    X(tx_queue_disabled, IONIC_LIF_TX_QUEUE_DISABLED)                         \
    X(tx_queue_error, IONIC_LIF_TX_QUEUE_ERROR)                               \
    X(tx_desc_fetch_error, IONIC_LIF_TX_DESC_FETCH_ERROR)                     \
    X(tx_desc_data_error, IONIC_LIF_TX_DESC_DATA_ERROR)                       \
    X(tx_queue_empty, IONIC_LIF_TX_QUEUE_EMPTY)                               \
    X(tx_rdma_ucast_bytes, IONIC_LIF_TX_RDMA_UCAST_BYTES)                     \
    X(tx_rdma_ucast_packets, IONIC_LIF_TX_RDMA_UCAST_PACKETS)                 \
    X(tx_rdma_mcast_bytes, IONIC_LIF_TX_RDMA_MCAST_BYTES)                     \
    X(tx_rdma_mcast_packets, IONIC_LIF_TX_RDMA_MCAST_PACKETS)                 \
    X(tx_rdma_cnp_packets, IONIC_LIF_TX_RDMA_CNP_PACKETS)                     \
    X(rx_rdma_ucast_bytes, IONIC_LIF_RX_RDMA_UCAST_BYTES)                     \
    X(rx_rdma_ucast_packets, IONIC_LIF_RX_RDMA_UCAST_PACKETS)                 \
    X(rx_rdma_mcast_bytes, IONIC_LIF_RX_RDMA_MCAST_BYTES)                     \
    X(rx_rdma_mcast_packets, IONIC_LIF_RX_RDMA_MCAST_PACKETS)                 \
    X(rx_rdma_cnp_packets, IONIC_LIF_RX_RDMA_CNP_PACKETS)                     \
    X(rx_rdma_ecn_packets, IONIC_LIF_RX_RDMA_ECN_PACKETS)                     \
    X(rdma_req_rx_pkt_seq_err, IONIC_LIF_RDMA_REQ_RX_PKT_SEQ_ERR)             \
    X(rdma_req_rx_rnr_retry_err, IONIC_LIF_RDMA_REQ_RX_RNR_RETRY_ERR)         \
    X(rdma_req_rx_remote_access_err, IONIC_LIF_RDMA_REQ_RX_REMOTE_ACCESS_ERR) \
    X(rdma_req_rx_remote_inv_req_err,                                         \
      IONIC_LIF_RDMA_REQ_RX_REMOTE_INV_REQ_ERR)                               \
    X(rdma_req_rx_remote_oper_err, IONIC_LIF_RDMA_REQ_RX_REMOTE_OPER_ERR)     \
    X(rdma_req_rx_implied_nak_seq_err,                                        \
      IONIC_LIF_RDMA_REQ_RX_IMPLIED_NAK_SEQ_ERR)                              \
    X(rdma_req_rx_cqe_err, IONIC_LIF_RDMA_REQ_RX_CQE_ERR)                     \
    X(rdma_req_rx_cqe_flush_err, IONIC_LIF_RDMA_REQ_RX_CQE_FLUSH_ERR)         \
    X(rdma_req_rx_dup_responses, IONIC_LIF_RDMA_REQ_RX_DUP_RESPONSES)         \
    X(rdma_req_rx_invalid_packets, IONIC_LIF_RDMA_REQ_RX_INVALID_PACKETS)     \
    X(rdma_req_tx_local_access_err, IONIC_LIF_RDMA_REQ_TX_LOCAL_ACCESS_ERR)   \
    X(rdma_req_tx_local_oper_err, IONIC_LIF_RDMA_REQ_TX_LOCAL_OPER_ERR)       \
    X(rdma_req_tx_memory_mgmt_err, IONIC_LIF_RDMA_REQ_TX_MEMORY_MGMT_ERR)     \
    X(rdma_resp_rx_dup_requests, IONIC_LIF_RDMA_RESP_RX_DUP_REQUESTS)         \
    X(rdma_resp_rx_out_of_buffer, IONIC_LIF_RDMA_RESP_RX_OUT_OF_BUFFER)       \
    X(rdma_resp_rx_out_of_seq_pkts, IONIC_LIF_RDMA_RESP_RX_OUT_OF_SEQ_PKTS)   \
    X(rdma_resp_rx_cqe_err, IONIC_LIF_RDMA_RESP_RX_CQE_ERR)                   \
    X(rdma_resp_rx_cqe_flush_err, IONIC_LIF_RDMA_RESP_RX_CQE_FLUSH_ERR)       \
    X(rdma_resp_rx_local_len_err, IONIC_LIF_RDMA_RESP_RX_LOCAL_LEN_ERR)       \
    X(rdma_resp_rx_inv_request_err, IONIC_LIF_RDMA_RESP_RX_INV_REQUEST_ERR)   \
    X(rdma_resp_rx_local_qp_oper_err,                                         \
      IONIC_LIF_RDMA_RESP_RX_LOCAL_QP_OPER_ERR)                               \
    X(rdma_resp_rx_out_of_atomic_resource,                                    \
      IONIC_LIF_RDMA_RESP_RX_OUT_OF_ATOMIC_RESOURCE)                          \
    X(rdma_resp_tx_pkt_seq_err, IONIC_LIF_RDMA_RESP_TX_PKT_SEQ_ERR)           \
    X(rdma_resp_tx_remote_inv_req_err,                                        \
      IONIC_LIF_RDMA_RESP_TX_REMOTE_INV_REQ_ERR)                              \
    X(rdma_resp_tx_remote_access_err,                                         \
      IONIC_LIF_RDMA_RESP_TX_REMOTE_ACCESS_ERR)                               \
    X(rdma_resp_tx_remote_oper_err, IONIC_LIF_RDMA_RESP_TX_REMOTE_OPER_ERR)   \
    X(rdma_resp_tx_rnr_retry_err, IONIC_LIF_RDMA_RESP_TX_RNR_RETRY_ERR)

static void test_layout(void)
{
    unsigned int named = 0, unnamed_ok = 0;
    char desc[128];

    check_u64("corner", "layout pin: offsetof(struct ionic_lif_info, stats)",
              "320", IONIC_LIF_INFO_STATS_OFF,
              offsetof(struct kernel_lif_info, stats));
    check_u64("corner", "layout pin: sizeof(struct ionic_lif_stats)", "1024",
              IONIC_LIF_STATS_BYTES, sizeof(struct kernel_lif_stats));

    /* One row per named field: its index, and the name the stats file uses. */
#define PIN(field, idx)                                                    \
    do {                                                                   \
        const char *nm = ionic_emu_lif_stat_name(idx);                     \
        snprintf(desc, sizeof(desc), "layout pin: %s", #field);            \
        check_u64("corner", desc, "offsetof(field) / 8",                   \
                  offsetof(struct kernel_lif_stats, field) / 8, (idx));    \
        snprintf(desc, sizeof(desc), "name of qword %u", (unsigned)(idx)); \
        check_u64("corner", desc, "\"" #field "\"", 0,                     \
                  (uint64_t)(nm == NULL || strcmp(nm, #field) != 0));      \
        named++;                                                           \
    } while (0);
    LIF_FIELDS(PIN)
#undef PIN

    /* Every other qword is reserved: no name, so the file and fill skip it. */
    for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++)
        if (ionic_emu_lif_stat_name(q))
            unnamed_ok++;
    check_u64("corner",
              "the named qwords are exactly the kernel's non-reserved fields",
              "as many names as LIF_FIELDS rows", named, unnamed_ok);
    check_u64(
        "negative", "name of qword 128 (out of range)", "NULL", 0,
        (uint64_t)(ionic_emu_lif_stat_name(IONIC_LIF_STATS_QWORDS) != NULL));
}

/*
 * struct ionic_port_stats and struct ionic_port_info up to it, copied from
 * the kernel's ionic_if.h (Linux 6.18).  Field order is upstream's, and it is
 * also ionic_port_stats_desc's, so these pins are `ethtool -S` order too.
 */
struct kernel_port_stats {
    uint64_t frames_rx_ok;
    uint64_t frames_rx_all;
    uint64_t frames_rx_bad_fcs;
    uint64_t frames_rx_bad_all;
    uint64_t octets_rx_ok;
    uint64_t octets_rx_all;
    uint64_t frames_rx_unicast;
    uint64_t frames_rx_multicast;
    uint64_t frames_rx_broadcast;
    uint64_t frames_rx_pause;
    uint64_t frames_rx_bad_length;
    uint64_t frames_rx_undersized;
    uint64_t frames_rx_oversized;
    uint64_t frames_rx_fragments;
    uint64_t frames_rx_jabber;
    uint64_t frames_rx_pripause;
    uint64_t frames_rx_stomped_crc;
    uint64_t frames_rx_too_long;
    uint64_t frames_rx_vlan_good;
    uint64_t frames_rx_dropped;
    uint64_t frames_rx_less_than_64b;
    uint64_t frames_rx_64b;
    uint64_t frames_rx_65b_127b;
    uint64_t frames_rx_128b_255b;
    uint64_t frames_rx_256b_511b;
    uint64_t frames_rx_512b_1023b;
    uint64_t frames_rx_1024b_1518b;
    uint64_t frames_rx_1519b_2047b;
    uint64_t frames_rx_2048b_4095b;
    uint64_t frames_rx_4096b_8191b;
    uint64_t frames_rx_8192b_9215b;
    uint64_t frames_rx_other;
    uint64_t frames_tx_ok;
    uint64_t frames_tx_all;
    uint64_t frames_tx_bad;
    uint64_t octets_tx_ok;
    uint64_t octets_tx_total;
    uint64_t frames_tx_unicast;
    uint64_t frames_tx_multicast;
    uint64_t frames_tx_broadcast;
    uint64_t frames_tx_pause;
    uint64_t frames_tx_pripause;
    uint64_t frames_tx_vlan;
    uint64_t frames_tx_less_than_64b;
    uint64_t frames_tx_64b;
    uint64_t frames_tx_65b_127b;
    uint64_t frames_tx_128b_255b;
    uint64_t frames_tx_256b_511b;
    uint64_t frames_tx_512b_1023b;
    uint64_t frames_tx_1024b_1518b;
    uint64_t frames_tx_1519b_2047b;
    uint64_t frames_tx_2048b_4095b;
    uint64_t frames_tx_4096b_8191b;
    uint64_t frames_tx_8192b_9215b;
    uint64_t frames_tx_other;
    uint64_t frames_tx_pri_0;
    uint64_t frames_tx_pri_1;
    uint64_t frames_tx_pri_2;
    uint64_t frames_tx_pri_3;
    uint64_t frames_tx_pri_4;
    uint64_t frames_tx_pri_5;
    uint64_t frames_tx_pri_6;
    uint64_t frames_tx_pri_7;
    uint64_t frames_rx_pri_0;
    uint64_t frames_rx_pri_1;
    uint64_t frames_rx_pri_2;
    uint64_t frames_rx_pri_3;
    uint64_t frames_rx_pri_4;
    uint64_t frames_rx_pri_5;
    uint64_t frames_rx_pri_6;
    uint64_t frames_rx_pri_7;
    uint64_t tx_pripause_0_1us_count;
    uint64_t tx_pripause_1_1us_count;
    uint64_t tx_pripause_2_1us_count;
    uint64_t tx_pripause_3_1us_count;
    uint64_t tx_pripause_4_1us_count;
    uint64_t tx_pripause_5_1us_count;
    uint64_t tx_pripause_6_1us_count;
    uint64_t tx_pripause_7_1us_count;
    uint64_t rx_pripause_0_1us_count;
    uint64_t rx_pripause_1_1us_count;
    uint64_t rx_pripause_2_1us_count;
    uint64_t rx_pripause_3_1us_count;
    uint64_t rx_pripause_4_1us_count;
    uint64_t rx_pripause_5_1us_count;
    uint64_t rx_pripause_6_1us_count;
    uint64_t rx_pripause_7_1us_count;
    uint64_t rx_pause_1us_count;
    uint64_t frames_tx_truncated;
};

struct kernel_port_info {
    uint8_t config[256];
    uint8_t status[320]; /* struct ionic_port_status, __packed */
    struct kernel_port_stats stats;
};

#define PORT_FIELDS(X)                                             \
    X(frames_rx_ok, IONIC_PORT_FRAMES_RX_OK)                       \
    X(frames_rx_all, IONIC_PORT_FRAMES_RX_ALL)                     \
    X(frames_rx_bad_fcs, IONIC_PORT_FRAMES_RX_BAD_FCS)             \
    X(frames_rx_bad_all, IONIC_PORT_FRAMES_RX_BAD_ALL)             \
    X(octets_rx_ok, IONIC_PORT_OCTETS_RX_OK)                       \
    X(octets_rx_all, IONIC_PORT_OCTETS_RX_ALL)                     \
    X(frames_rx_unicast, IONIC_PORT_FRAMES_RX_UNICAST)             \
    X(frames_rx_multicast, IONIC_PORT_FRAMES_RX_MULTICAST)         \
    X(frames_rx_broadcast, IONIC_PORT_FRAMES_RX_BROADCAST)         \
    X(frames_rx_pause, IONIC_PORT_FRAMES_RX_PAUSE)                 \
    X(frames_rx_bad_length, IONIC_PORT_FRAMES_RX_BAD_LENGTH)       \
    X(frames_rx_undersized, IONIC_PORT_FRAMES_RX_UNDERSIZED)       \
    X(frames_rx_oversized, IONIC_PORT_FRAMES_RX_OVERSIZED)         \
    X(frames_rx_fragments, IONIC_PORT_FRAMES_RX_FRAGMENTS)         \
    X(frames_rx_jabber, IONIC_PORT_FRAMES_RX_JABBER)               \
    X(frames_rx_pripause, IONIC_PORT_FRAMES_RX_PRIPAUSE)           \
    X(frames_rx_stomped_crc, IONIC_PORT_FRAMES_RX_STOMPED_CRC)     \
    X(frames_rx_too_long, IONIC_PORT_FRAMES_RX_TOO_LONG)           \
    X(frames_rx_vlan_good, IONIC_PORT_FRAMES_RX_VLAN_GOOD)         \
    X(frames_rx_dropped, IONIC_PORT_FRAMES_RX_DROPPED)             \
    X(frames_rx_less_than_64b, IONIC_PORT_FRAMES_RX_LESS_THAN_64B) \
    X(frames_rx_64b, IONIC_PORT_FRAMES_RX_64B)                     \
    X(frames_rx_65b_127b, IONIC_PORT_FRAMES_RX_65B_127B)           \
    X(frames_rx_128b_255b, IONIC_PORT_FRAMES_RX_128B_255B)         \
    X(frames_rx_256b_511b, IONIC_PORT_FRAMES_RX_256B_511B)         \
    X(frames_rx_512b_1023b, IONIC_PORT_FRAMES_RX_512B_1023B)       \
    X(frames_rx_1024b_1518b, IONIC_PORT_FRAMES_RX_1024B_1518B)     \
    X(frames_rx_1519b_2047b, IONIC_PORT_FRAMES_RX_1519B_2047B)     \
    X(frames_rx_2048b_4095b, IONIC_PORT_FRAMES_RX_2048B_4095B)     \
    X(frames_rx_4096b_8191b, IONIC_PORT_FRAMES_RX_4096B_8191B)     \
    X(frames_rx_8192b_9215b, IONIC_PORT_FRAMES_RX_8192B_9215B)     \
    X(frames_rx_other, IONIC_PORT_FRAMES_RX_OTHER)                 \
    X(frames_tx_ok, IONIC_PORT_FRAMES_TX_OK)                       \
    X(frames_tx_all, IONIC_PORT_FRAMES_TX_ALL)                     \
    X(frames_tx_bad, IONIC_PORT_FRAMES_TX_BAD)                     \
    X(octets_tx_ok, IONIC_PORT_OCTETS_TX_OK)                       \
    X(octets_tx_total, IONIC_PORT_OCTETS_TX_TOTAL)                 \
    X(frames_tx_unicast, IONIC_PORT_FRAMES_TX_UNICAST)             \
    X(frames_tx_multicast, IONIC_PORT_FRAMES_TX_MULTICAST)         \
    X(frames_tx_broadcast, IONIC_PORT_FRAMES_TX_BROADCAST)         \
    X(frames_tx_pause, IONIC_PORT_FRAMES_TX_PAUSE)                 \
    X(frames_tx_pripause, IONIC_PORT_FRAMES_TX_PRIPAUSE)           \
    X(frames_tx_vlan, IONIC_PORT_FRAMES_TX_VLAN)                   \
    X(frames_tx_less_than_64b, IONIC_PORT_FRAMES_TX_LESS_THAN_64B) \
    X(frames_tx_64b, IONIC_PORT_FRAMES_TX_64B)                     \
    X(frames_tx_65b_127b, IONIC_PORT_FRAMES_TX_65B_127B)           \
    X(frames_tx_128b_255b, IONIC_PORT_FRAMES_TX_128B_255B)         \
    X(frames_tx_256b_511b, IONIC_PORT_FRAMES_TX_256B_511B)         \
    X(frames_tx_512b_1023b, IONIC_PORT_FRAMES_TX_512B_1023B)       \
    X(frames_tx_1024b_1518b, IONIC_PORT_FRAMES_TX_1024B_1518B)     \
    X(frames_tx_1519b_2047b, IONIC_PORT_FRAMES_TX_1519B_2047B)     \
    X(frames_tx_2048b_4095b, IONIC_PORT_FRAMES_TX_2048B_4095B)     \
    X(frames_tx_4096b_8191b, IONIC_PORT_FRAMES_TX_4096B_8191B)     \
    X(frames_tx_8192b_9215b, IONIC_PORT_FRAMES_TX_8192B_9215B)     \
    X(frames_tx_other, IONIC_PORT_FRAMES_TX_OTHER)                 \
    X(frames_tx_pri_0, IONIC_PORT_FRAMES_TX_PRI_0)                 \
    X(frames_tx_pri_1, IONIC_PORT_FRAMES_TX_PRI_1)                 \
    X(frames_tx_pri_2, IONIC_PORT_FRAMES_TX_PRI_2)                 \
    X(frames_tx_pri_3, IONIC_PORT_FRAMES_TX_PRI_3)                 \
    X(frames_tx_pri_4, IONIC_PORT_FRAMES_TX_PRI_4)                 \
    X(frames_tx_pri_5, IONIC_PORT_FRAMES_TX_PRI_5)                 \
    X(frames_tx_pri_6, IONIC_PORT_FRAMES_TX_PRI_6)                 \
    X(frames_tx_pri_7, IONIC_PORT_FRAMES_TX_PRI_7)                 \
    X(frames_rx_pri_0, IONIC_PORT_FRAMES_RX_PRI_0)                 \
    X(frames_rx_pri_1, IONIC_PORT_FRAMES_RX_PRI_1)                 \
    X(frames_rx_pri_2, IONIC_PORT_FRAMES_RX_PRI_2)                 \
    X(frames_rx_pri_3, IONIC_PORT_FRAMES_RX_PRI_3)                 \
    X(frames_rx_pri_4, IONIC_PORT_FRAMES_RX_PRI_4)                 \
    X(frames_rx_pri_5, IONIC_PORT_FRAMES_RX_PRI_5)                 \
    X(frames_rx_pri_6, IONIC_PORT_FRAMES_RX_PRI_6)                 \
    X(frames_rx_pri_7, IONIC_PORT_FRAMES_RX_PRI_7)                 \
    X(tx_pripause_0_1us_count, IONIC_PORT_TX_PRIPAUSE_0_1US_COUNT) \
    X(tx_pripause_1_1us_count, IONIC_PORT_TX_PRIPAUSE_1_1US_COUNT) \
    X(tx_pripause_2_1us_count, IONIC_PORT_TX_PRIPAUSE_2_1US_COUNT) \
    X(tx_pripause_3_1us_count, IONIC_PORT_TX_PRIPAUSE_3_1US_COUNT) \
    X(tx_pripause_4_1us_count, IONIC_PORT_TX_PRIPAUSE_4_1US_COUNT) \
    X(tx_pripause_5_1us_count, IONIC_PORT_TX_PRIPAUSE_5_1US_COUNT) \
    X(tx_pripause_6_1us_count, IONIC_PORT_TX_PRIPAUSE_6_1US_COUNT) \
    X(tx_pripause_7_1us_count, IONIC_PORT_TX_PRIPAUSE_7_1US_COUNT) \
    X(rx_pripause_0_1us_count, IONIC_PORT_RX_PRIPAUSE_0_1US_COUNT) \
    X(rx_pripause_1_1us_count, IONIC_PORT_RX_PRIPAUSE_1_1US_COUNT) \
    X(rx_pripause_2_1us_count, IONIC_PORT_RX_PRIPAUSE_2_1US_COUNT) \
    X(rx_pripause_3_1us_count, IONIC_PORT_RX_PRIPAUSE_3_1US_COUNT) \
    X(rx_pripause_4_1us_count, IONIC_PORT_RX_PRIPAUSE_4_1US_COUNT) \
    X(rx_pripause_5_1us_count, IONIC_PORT_RX_PRIPAUSE_5_1US_COUNT) \
    X(rx_pripause_6_1us_count, IONIC_PORT_RX_PRIPAUSE_6_1US_COUNT) \
    X(rx_pripause_7_1us_count, IONIC_PORT_RX_PRIPAUSE_7_1US_COUNT) \
    X(rx_pause_1us_count, IONIC_PORT_RX_PAUSE_1US_COUNT)           \
    X(frames_tx_truncated, IONIC_PORT_FRAMES_TX_TRUNCATED)

static void test_port_layout(void)
{
    unsigned int pinned = 0;
    char desc[128];

    check_u64("corner", "layout pin: offsetof(struct ionic_port_info, stats)",
              "576", IONIC_PORT_INFO_STATS_OFF,
              offsetof(struct kernel_port_info, stats));
    check_u64("corner", "layout pin: sizeof(struct ionic_port_stats)", "712",
              IONIC_PORT_STATS_BYTES, sizeof(struct kernel_port_stats));

#define PIN(field, idx)                                                  \
    do {                                                                 \
        const char *nm = ionic_emu_port_stat_name(idx);                  \
        snprintf(desc, sizeof(desc), "port layout pin: %s", #field);     \
        check_u64("corner", desc, "offsetof(field) / 8",                 \
                  offsetof(struct kernel_port_stats, field) / 8, (idx)); \
        snprintf(desc, sizeof(desc), "name of port qword %u",            \
                 (unsigned)(idx));                                       \
        check_u64("corner", desc, "\"" #field "\"", 0,                   \
                  (uint64_t)(nm == NULL || strcmp(nm, #field) != 0));    \
        pinned++;                                                        \
    } while (0);
    PORT_FIELDS(PIN)
#undef PIN

    check_u64("corner", "every port qword is pinned", "89", 89, pinned);
    check_u64(
        "negative", "name of port qword 89 (out of range)", "NULL", 0,
        (uint64_t)(ionic_emu_port_stat_name(IONIC_PORT_STATS_QWORDS) != NULL));
}

int main(void)
{
    test_classify();
    test_counting();
    test_fill();
    test_layout();
    test_port_counting();
    test_port_buckets();
    test_port_misc();
    test_port_inbox_full();
    test_port_clear();
    test_port_layout();

    if (failures)
        printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
