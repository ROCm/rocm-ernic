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

#include <endian.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

#include "ionic_emu_stats.h"

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
     "mesh inbox full, after fold",
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
     "inbox full 1000 times, then fold",
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
        ionic_emu_stats_rx_inbox_full(st, f, c->in.len);
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
        ionic_emu_stats_fold(&st);

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

/* ---- Fill, fold, reset, wrap ------------------------------------------ */

static uint64_t qword_at(const uint8_t *buf, unsigned int idx)
{
    uint64_t v;

    memcpy(&v, buf + (size_t)idx * 8, 8);
    return le64toh(v);
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
    ionic_emu_stats_rx_inbox_full(&st, frame_to(mac_bcast, 64), 64);
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
                  (uint64_t)memcmp(out + IONIC_LIF_TX_UCAST_BYTES * 8, le, 8));
    }

    /* corner: reset() zeroes every qword, including the atomics */
    for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++)
        st.lif[q] = q + 1;
    ionic_emu_stats_rx_inbox_full(&st, frame_to(mac_ucast, 64), 64);
    ionic_emu_stats_reset(&st);
    ionic_emu_stats_fill(&st, out);
    {
        uint64_t nonzero = 0;

        for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++)
            nonzero += qword_at(out, q) != 0;
        check_u64("corner", "reset(), then fill()",
                  "every qword 0, including the folded inbox atomic", 0,
                  nonzero);
    }

    /* corner: folding twice counts the inbox drop once */
    ionic_emu_stats_reset(&st);
    ionic_emu_stats_rx_inbox_full(&st, frame_to(mac_ucast, 64), 64);
    ionic_emu_stats_fold(&st);
    ionic_emu_stats_fold(&st);
    ionic_emu_stats_fill(&st, out);
    check_u64("corner", "inbox-full atomic folded twice (and once by fill)",
              "counted once", 1,
              qword_at(out, IONIC_LIF_RX_UCAST_DROP_PACKETS));
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

struct producer {
    struct ionic_emu_stats *stats;
    atomic_bool *start;
    atomic_uint *done;
    const uint8_t *frame;
    unsigned int count;
};

static void *produce_drops(void *opaque)
{
    struct producer *p = opaque;

    while (!atomic_load(p->start))
        sched_yield();
    for (unsigned int i = 0; i < p->count; i++)
        ionic_emu_stats_rx_inbox_full(p->stats, p->frame, 64);
    atomic_fetch_add(p->done, 1);
    return NULL;
}

static void test_concurrent_fold(void)
{
    static const struct {
        const char *description;
        unsigned int count;
    } cases[] = {{"boundary: zero events", 0},
                 {"boundary: one event per class", 1},
                 {"corner: three producers race the folder", 20000}};
    static const unsigned int indices[] = {IONIC_LIF_RX_UCAST_DROP_BYTES,
                                           IONIC_LIF_RX_MCAST_DROP_BYTES,
                                           IONIC_LIF_RX_BCAST_DROP_BYTES};
    uint8_t frames[3][64] = {{0}};

    memcpy(frames[0], mac_ucast, 6);
    memcpy(frames[1], mac_mcast, 6);
    memcpy(frames[2], mac_bcast, 6);
    for (size_t row = 0; row < sizeof(cases) / sizeof(cases[0]); row++) {
        struct ionic_emu_stats st;
        atomic_bool start = false;
        atomic_uint done = 0;
        struct producer args[3];
        pthread_t threads[3];
        unsigned int created = 0;

        ionic_emu_stats_reset(&st);
        for (unsigned int c = 0; c < 3; c++) {
            args[c] = (struct producer){&st, &start, &done, frames[c],
                                        cases[row].count};
            if (pthread_create(&threads[c], NULL, produce_drops, &args[c]) != 0)
                break;
            created++;
        }
        atomic_store(&start, true);
        while (atomic_load(&done) < created) {
            ionic_emu_stats_fold(&st);
            sched_yield();
        }
        for (unsigned int c = 0; c < created; c++) {
            void *result = NULL;

            if (pthread_join(threads[c], &result) != 0 || result != NULL)
                failures++;
        }
        check_u64("positive", cases[row].description, "all producers started",
                  3, created);
        ionic_emu_stats_fold(&st);
        ionic_emu_stats_fold(&st);
        for (unsigned int c = 0; c < 3; c++) {
            check_u64("corner", cases[row].description,
                      "exact byte total after final fold",
                      (uint64_t)cases[row].count * 64, st.lif[indices[c]]);
            check_u64("corner", cases[row].description,
                      "exact packet total, never counted twice",
                      cases[row].count, st.lif[indices[c] + 1]);
            check_u64("corner", cases[row].description, "pending bytes empty",
                      0, atomic_load(&st.inbox_drop_bytes[c]));
            check_u64("corner", cases[row].description, "pending packets empty",
                      0, atomic_load(&st.inbox_drop_packets[c]));
        }
    }
}

int main(void)
{
    test_concurrent_fold();
    test_classify();
    test_counting();
    test_fill();
    test_layout();

    if (failures)
        printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
