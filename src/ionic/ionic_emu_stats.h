/*
 * Counters for the emulated ionic NIC's statistics blocks.
 *
 * The ionic driver never asks the device for statistics.  It reads them
 * straight out of DMA memory it allocated: struct ionic_lif_stats sits at
 * byte 320 of the struct ionic_lif_info whose address LIF_INIT passes, and
 * ionic_get_stats64() turns it into what `ip -s link`, /proc/net/dev and
 * IFLA_STATS64 report.  struct ionic_port_stats, the MAC's counters that
 * `ethtool -S` lists, sits at byte 576 of the struct ionic_port_info that
 * PORT_INIT passes.  The device has to keep both blocks current itself.
 *
 * struct ionic_emu_stats is the emulator's copy.  The datapath counts each
 * event once, here; ionic_emu_stats_fill() renders the guest's little-endian
 * block from it, and the --stats-file writer prints the same values, so a
 * test can compare what the guest sees with what the emulator counted.
 *
 * enum ionic_lif_stat and enum ionic_port_stat are the only places that know
 * where each counter sits in its block.  Their values are qword indices,
 * copied from struct ionic_lif_stats and struct ionic_port_stats in the
 * kernel's ionic_if.h; the LIF block's reserved slots are never written, so
 * they stay zero as the driver expects.
 *
 * Everything here runs on the main loop thread.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef IONIC_EMU_STATS_H
#define IONIC_EMU_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* struct ionic_lif_info: config[256], status[64], then the stats. */
#define IONIC_LIF_INFO_STATS_OFF 320u

/* struct ionic_lif_stats is 128 __le64 (1024 bytes). */
#define IONIC_LIF_STATS_QWORDS 128u
#define IONIC_LIF_STATS_BYTES  1024u
_Static_assert(IONIC_LIF_STATS_BYTES == IONIC_LIF_STATS_QWORDS * 8u,
               "struct ionic_lif_stats is 128 qwords");

/* Qword index of each named counter in struct ionic_lif_stats. */
enum ionic_lif_stat {
    IONIC_LIF_RX_UCAST_BYTES = 0,
    IONIC_LIF_RX_UCAST_PACKETS = 1,
    IONIC_LIF_RX_MCAST_BYTES = 2,
    IONIC_LIF_RX_MCAST_PACKETS = 3,
    IONIC_LIF_RX_BCAST_BYTES = 4,
    IONIC_LIF_RX_BCAST_PACKETS = 5,

    IONIC_LIF_RX_UCAST_DROP_BYTES = 8,
    IONIC_LIF_RX_UCAST_DROP_PACKETS = 9,
    IONIC_LIF_RX_MCAST_DROP_BYTES = 10,
    IONIC_LIF_RX_MCAST_DROP_PACKETS = 11,
    IONIC_LIF_RX_BCAST_DROP_BYTES = 12,
    IONIC_LIF_RX_BCAST_DROP_PACKETS = 13,
    IONIC_LIF_RX_DMA_ERROR = 14,

    IONIC_LIF_TX_UCAST_BYTES = 16,
    IONIC_LIF_TX_UCAST_PACKETS = 17,
    IONIC_LIF_TX_MCAST_BYTES = 18,
    IONIC_LIF_TX_MCAST_PACKETS = 19,
    IONIC_LIF_TX_BCAST_BYTES = 20,
    IONIC_LIF_TX_BCAST_PACKETS = 21,

    IONIC_LIF_TX_UCAST_DROP_BYTES = 24,
    IONIC_LIF_TX_UCAST_DROP_PACKETS = 25,
    IONIC_LIF_TX_MCAST_DROP_BYTES = 26,
    IONIC_LIF_TX_MCAST_DROP_PACKETS = 27,
    IONIC_LIF_TX_BCAST_DROP_BYTES = 28,
    IONIC_LIF_TX_BCAST_DROP_PACKETS = 29,
    IONIC_LIF_TX_DMA_ERROR = 30,

    IONIC_LIF_RX_QUEUE_DISABLED = 32,
    IONIC_LIF_RX_QUEUE_EMPTY = 33,
    IONIC_LIF_RX_QUEUE_ERROR = 34,
    IONIC_LIF_RX_DESC_FETCH_ERROR = 35,
    IONIC_LIF_RX_DESC_DATA_ERROR = 36,

    IONIC_LIF_TX_QUEUE_DISABLED = 40,
    IONIC_LIF_TX_QUEUE_ERROR = 41,
    IONIC_LIF_TX_DESC_FETCH_ERROR = 42,
    IONIC_LIF_TX_DESC_DATA_ERROR = 43,
    IONIC_LIF_TX_QUEUE_EMPTY = 44,

    IONIC_LIF_TX_RDMA_UCAST_BYTES = 48,
    IONIC_LIF_TX_RDMA_UCAST_PACKETS = 49,
    IONIC_LIF_TX_RDMA_MCAST_BYTES = 50,
    IONIC_LIF_TX_RDMA_MCAST_PACKETS = 51,
    IONIC_LIF_TX_RDMA_CNP_PACKETS = 52,

    IONIC_LIF_RX_RDMA_UCAST_BYTES = 56,
    IONIC_LIF_RX_RDMA_UCAST_PACKETS = 57,
    IONIC_LIF_RX_RDMA_MCAST_BYTES = 58,
    IONIC_LIF_RX_RDMA_MCAST_PACKETS = 59,
    IONIC_LIF_RX_RDMA_CNP_PACKETS = 60,
    IONIC_LIF_RX_RDMA_ECN_PACKETS = 61,

    /* RDMA requester errors, bytes 768..895. */
    IONIC_LIF_RDMA_REQ_RX_PKT_SEQ_ERR = 96,
    IONIC_LIF_RDMA_REQ_RX_RNR_RETRY_ERR = 97,
    IONIC_LIF_RDMA_REQ_RX_REMOTE_ACCESS_ERR = 98,
    IONIC_LIF_RDMA_REQ_RX_REMOTE_INV_REQ_ERR = 99,
    IONIC_LIF_RDMA_REQ_RX_REMOTE_OPER_ERR = 100,
    IONIC_LIF_RDMA_REQ_RX_IMPLIED_NAK_SEQ_ERR = 101,
    IONIC_LIF_RDMA_REQ_RX_CQE_ERR = 102,
    IONIC_LIF_RDMA_REQ_RX_CQE_FLUSH_ERR = 103,
    IONIC_LIF_RDMA_REQ_RX_DUP_RESPONSES = 104,
    IONIC_LIF_RDMA_REQ_RX_INVALID_PACKETS = 105,
    IONIC_LIF_RDMA_REQ_TX_LOCAL_ACCESS_ERR = 106,
    IONIC_LIF_RDMA_REQ_TX_LOCAL_OPER_ERR = 107,
    IONIC_LIF_RDMA_REQ_TX_MEMORY_MGMT_ERR = 108,

    /* RDMA responder errors, bytes 896..1023. */
    IONIC_LIF_RDMA_RESP_RX_DUP_REQUESTS = 112,
    IONIC_LIF_RDMA_RESP_RX_OUT_OF_BUFFER = 113,
    IONIC_LIF_RDMA_RESP_RX_OUT_OF_SEQ_PKTS = 114,
    IONIC_LIF_RDMA_RESP_RX_CQE_ERR = 115,
    IONIC_LIF_RDMA_RESP_RX_CQE_FLUSH_ERR = 116,
    IONIC_LIF_RDMA_RESP_RX_LOCAL_LEN_ERR = 117,
    IONIC_LIF_RDMA_RESP_RX_INV_REQUEST_ERR = 118,
    IONIC_LIF_RDMA_RESP_RX_LOCAL_QP_OPER_ERR = 119,
    IONIC_LIF_RDMA_RESP_RX_OUT_OF_ATOMIC_RESOURCE = 120,
    IONIC_LIF_RDMA_RESP_TX_PKT_SEQ_ERR = 121,
    IONIC_LIF_RDMA_RESP_TX_REMOTE_INV_REQ_ERR = 122,
    IONIC_LIF_RDMA_RESP_TX_REMOTE_ACCESS_ERR = 123,
    IONIC_LIF_RDMA_RESP_TX_REMOTE_OPER_ERR = 124,
    IONIC_LIF_RDMA_RESP_TX_RNR_RETRY_ERR = 125,
};

/* struct ionic_port_info: config[256], status[320], then the stats. */
#define IONIC_PORT_INFO_STATS_OFF 576u

/* struct ionic_port_stats is 89 __le64 (712 bytes), none reserved. */
#define IONIC_PORT_STATS_QWORDS 89u
#define IONIC_PORT_STATS_BYTES  712u
_Static_assert(IONIC_PORT_STATS_BYTES == IONIC_PORT_STATS_QWORDS * 8u,
               "struct ionic_port_stats is 89 qwords");

/*
 * Every counter in struct ionic_port_stats, in ionic_if.h order, which is
 * also the driver's ionic_port_stats_desc order and so the order `ethtool -S`
 * lists them in: X(enum suffix, name).
 */
#define IONIC_PORT_STATS(X)                             \
    X(FRAMES_RX_OK, frames_rx_ok)                       \
    X(FRAMES_RX_ALL, frames_rx_all)                     \
    X(FRAMES_RX_BAD_FCS, frames_rx_bad_fcs)             \
    X(FRAMES_RX_BAD_ALL, frames_rx_bad_all)             \
    X(OCTETS_RX_OK, octets_rx_ok)                       \
    X(OCTETS_RX_ALL, octets_rx_all)                     \
    X(FRAMES_RX_UNICAST, frames_rx_unicast)             \
    X(FRAMES_RX_MULTICAST, frames_rx_multicast)         \
    X(FRAMES_RX_BROADCAST, frames_rx_broadcast)         \
    X(FRAMES_RX_PAUSE, frames_rx_pause)                 \
    X(FRAMES_RX_BAD_LENGTH, frames_rx_bad_length)       \
    X(FRAMES_RX_UNDERSIZED, frames_rx_undersized)       \
    X(FRAMES_RX_OVERSIZED, frames_rx_oversized)         \
    X(FRAMES_RX_FRAGMENTS, frames_rx_fragments)         \
    X(FRAMES_RX_JABBER, frames_rx_jabber)               \
    X(FRAMES_RX_PRIPAUSE, frames_rx_pripause)           \
    X(FRAMES_RX_STOMPED_CRC, frames_rx_stomped_crc)     \
    X(FRAMES_RX_TOO_LONG, frames_rx_too_long)           \
    X(FRAMES_RX_VLAN_GOOD, frames_rx_vlan_good)         \
    X(FRAMES_RX_DROPPED, frames_rx_dropped)             \
    X(FRAMES_RX_LESS_THAN_64B, frames_rx_less_than_64b) \
    X(FRAMES_RX_64B, frames_rx_64b)                     \
    X(FRAMES_RX_65B_127B, frames_rx_65b_127b)           \
    X(FRAMES_RX_128B_255B, frames_rx_128b_255b)         \
    X(FRAMES_RX_256B_511B, frames_rx_256b_511b)         \
    X(FRAMES_RX_512B_1023B, frames_rx_512b_1023b)       \
    X(FRAMES_RX_1024B_1518B, frames_rx_1024b_1518b)     \
    X(FRAMES_RX_1519B_2047B, frames_rx_1519b_2047b)     \
    X(FRAMES_RX_2048B_4095B, frames_rx_2048b_4095b)     \
    X(FRAMES_RX_4096B_8191B, frames_rx_4096b_8191b)     \
    X(FRAMES_RX_8192B_9215B, frames_rx_8192b_9215b)     \
    X(FRAMES_RX_OTHER, frames_rx_other)                 \
    X(FRAMES_TX_OK, frames_tx_ok)                       \
    X(FRAMES_TX_ALL, frames_tx_all)                     \
    X(FRAMES_TX_BAD, frames_tx_bad)                     \
    X(OCTETS_TX_OK, octets_tx_ok)                       \
    X(OCTETS_TX_TOTAL, octets_tx_total)                 \
    X(FRAMES_TX_UNICAST, frames_tx_unicast)             \
    X(FRAMES_TX_MULTICAST, frames_tx_multicast)         \
    X(FRAMES_TX_BROADCAST, frames_tx_broadcast)         \
    X(FRAMES_TX_PAUSE, frames_tx_pause)                 \
    X(FRAMES_TX_PRIPAUSE, frames_tx_pripause)           \
    X(FRAMES_TX_VLAN, frames_tx_vlan)                   \
    X(FRAMES_TX_LESS_THAN_64B, frames_tx_less_than_64b) \
    X(FRAMES_TX_64B, frames_tx_64b)                     \
    X(FRAMES_TX_65B_127B, frames_tx_65b_127b)           \
    X(FRAMES_TX_128B_255B, frames_tx_128b_255b)         \
    X(FRAMES_TX_256B_511B, frames_tx_256b_511b)         \
    X(FRAMES_TX_512B_1023B, frames_tx_512b_1023b)       \
    X(FRAMES_TX_1024B_1518B, frames_tx_1024b_1518b)     \
    X(FRAMES_TX_1519B_2047B, frames_tx_1519b_2047b)     \
    X(FRAMES_TX_2048B_4095B, frames_tx_2048b_4095b)     \
    X(FRAMES_TX_4096B_8191B, frames_tx_4096b_8191b)     \
    X(FRAMES_TX_8192B_9215B, frames_tx_8192b_9215b)     \
    X(FRAMES_TX_OTHER, frames_tx_other)                 \
    X(FRAMES_TX_PRI_0, frames_tx_pri_0)                 \
    X(FRAMES_TX_PRI_1, frames_tx_pri_1)                 \
    X(FRAMES_TX_PRI_2, frames_tx_pri_2)                 \
    X(FRAMES_TX_PRI_3, frames_tx_pri_3)                 \
    X(FRAMES_TX_PRI_4, frames_tx_pri_4)                 \
    X(FRAMES_TX_PRI_5, frames_tx_pri_5)                 \
    X(FRAMES_TX_PRI_6, frames_tx_pri_6)                 \
    X(FRAMES_TX_PRI_7, frames_tx_pri_7)                 \
    X(FRAMES_RX_PRI_0, frames_rx_pri_0)                 \
    X(FRAMES_RX_PRI_1, frames_rx_pri_1)                 \
    X(FRAMES_RX_PRI_2, frames_rx_pri_2)                 \
    X(FRAMES_RX_PRI_3, frames_rx_pri_3)                 \
    X(FRAMES_RX_PRI_4, frames_rx_pri_4)                 \
    X(FRAMES_RX_PRI_5, frames_rx_pri_5)                 \
    X(FRAMES_RX_PRI_6, frames_rx_pri_6)                 \
    X(FRAMES_RX_PRI_7, frames_rx_pri_7)                 \
    X(TX_PRIPAUSE_0_1US_COUNT, tx_pripause_0_1us_count) \
    X(TX_PRIPAUSE_1_1US_COUNT, tx_pripause_1_1us_count) \
    X(TX_PRIPAUSE_2_1US_COUNT, tx_pripause_2_1us_count) \
    X(TX_PRIPAUSE_3_1US_COUNT, tx_pripause_3_1us_count) \
    X(TX_PRIPAUSE_4_1US_COUNT, tx_pripause_4_1us_count) \
    X(TX_PRIPAUSE_5_1US_COUNT, tx_pripause_5_1us_count) \
    X(TX_PRIPAUSE_6_1US_COUNT, tx_pripause_6_1us_count) \
    X(TX_PRIPAUSE_7_1US_COUNT, tx_pripause_7_1us_count) \
    X(RX_PRIPAUSE_0_1US_COUNT, rx_pripause_0_1us_count) \
    X(RX_PRIPAUSE_1_1US_COUNT, rx_pripause_1_1us_count) \
    X(RX_PRIPAUSE_2_1US_COUNT, rx_pripause_2_1us_count) \
    X(RX_PRIPAUSE_3_1US_COUNT, rx_pripause_3_1us_count) \
    X(RX_PRIPAUSE_4_1US_COUNT, rx_pripause_4_1us_count) \
    X(RX_PRIPAUSE_5_1US_COUNT, rx_pripause_5_1us_count) \
    X(RX_PRIPAUSE_6_1US_COUNT, rx_pripause_6_1us_count) \
    X(RX_PRIPAUSE_7_1US_COUNT, rx_pripause_7_1us_count) \
    X(RX_PAUSE_1US_COUNT, rx_pause_1us_count)           \
    X(FRAMES_TX_TRUNCATED, frames_tx_truncated)

/* Qword index of each counter in struct ionic_port_stats. */
enum ionic_port_stat {
#define IONIC_PORT_STAT_ENUM(id, name) IONIC_PORT_##id,
    IONIC_PORT_STATS(IONIC_PORT_STAT_ENUM)
#undef IONIC_PORT_STAT_ENUM
        IONIC_PORT_STAT_COUNT
};
_Static_assert(IONIC_PORT_STAT_COUNT == IONIC_PORT_STATS_QWORDS,
               "one IONIC_PORT_STATS entry per qword");

/*
 * A MAC pads a frame shorter than this (FCS excluded) before it goes on the
 * wire, and counts every frame with its 4-byte FCS, which the emulator's
 * frames do not carry.
 */
#define IONIC_EMU_MIN_FRAME 60u
#define IONIC_EMU_FCS_LEN   4u

/* Destination MAC class: the group bit, with all-ones as broadcast. */
enum ionic_emu_mac_class {
    IONIC_EMU_UCAST = 0,
    IONIC_EMU_MCAST = 1,
    IONIC_EMU_BCAST = 2,
    IONIC_EMU_MAC_CLASSES = 3,
};

/* Why the datapath dropped a frame; each maps to one queue/ring counter. */
enum ionic_emu_drop {
    IONIC_EMU_DROP_NO_BUFFER,  /* rx: the guest posted no buffer */
    IONIC_EMU_DROP_TOO_BIG,    /* rx: frame larger than the buffer */
    IONIC_EMU_DROP_DESC_FETCH, /* the descriptor could not be read */
    IONIC_EMU_DROP_DESC_DATA,  /* the descriptor made no sense */
    IONIC_EMU_DROP_DMA,        /* frame or completion DMA failed */
    IONIC_EMU_DROP_SEND,       /* tx: the host backend refused it */
    IONIC_EMU_DROP_DISABLED,   /* doorbell on a queue not initialized */
    IONIC_EMU_DROP_INJECTED,   /* an injected fault (ionic_fault.h) took it */
};

struct ionic_emu_stats {
    /* Host-order copy of struct ionic_lif_stats, indexed by ionic_lif_stat. */
    uint64_t lif[IONIC_LIF_STATS_QWORDS];

    /* Host-order copy of struct ionic_port_stats, by ionic_port_stat. */
    uint64_t port[IONIC_PORT_STATS_QWORDS];
};

/*
 * The class of a frame by its destination MAC, or -1 if @len is too short
 * to hold one.
 */
int ionic_emu_mac_class(const uint8_t *frame, size_t len);

/* Zero every counter. */
void ionic_emu_stats_reset(struct ionic_emu_stats *st);

/* A frame the guest sent (@is_tx) or received, @len bytes on the wire. */
void ionic_emu_stats_frame(struct ionic_emu_stats *st, const uint8_t *frame,
                           size_t len, bool is_tx);

/*
 * A frame dropped for @why.  @frame may be NULL when the drop happened
 * before the frame was read (a descriptor or DMA failure); then only the
 * queue/ring counter moves.
 */
void ionic_emu_stats_drop(struct ionic_emu_stats *st, const uint8_t *frame,
                          size_t len, bool is_tx, enum ionic_emu_drop why);

/*
 * The Rx inbox (ionic_eth_emu_queue_rx_frame()) was full, so the frame is
 * gone.  The MAC took it in, as
 * ionic_emu_port_frame() counts it with @max_frame, and the LIF had nowhere
 * to put it, unless the MAC refused it first for being too long.
 */
void ionic_emu_stats_rx_inbox_full(struct ionic_emu_stats *st,
                                   const uint8_t *frame, size_t len,
                                   size_t max_frame);

/* One RDMA message of @bytes moved by the RDMA datapath. */
void ionic_emu_stats_rdma(struct ionic_emu_stats *st, uint64_t bytes,
                          bool is_tx);

/*
 * A frame crossing the port: sent (@is_tx), or received from the wire, which
 * here is the TAP or an in-process endpoint's queue.  Counts the MAC's view
 * -- frame and octet totals with FCS, size bucket, destination class, VLAN
 * tag and priority.  A received frame longer than @max_frame (FCS excluded)
 * is a MAC error: it is counted as too long and the function returns false,
 * and the caller drops it.  @max_frame is ignored for tx.  Returns true
 * otherwise.
 */
bool ionic_emu_port_frame(struct ionic_emu_stats *st, const uint8_t *frame,
                          size_t len, bool is_tx, size_t max_frame);

/* A frame the MAC took in but the LIF dropped (any ionic_emu_drop on rx). */
void ionic_emu_port_rx_dropped(struct ionic_emu_stats *st);

/*
 * PORT_RESET: zero the port counters.  The LIF counters are kept, as
 * LIF_RESET is a separate command.
 */
void ionic_emu_port_clear(struct ionic_emu_stats *st);

/*
 * Render struct ionic_lif_stats into @out (little-endian, every reserved
 * qword zero).
 */
void ionic_emu_stats_fill(const struct ionic_emu_stats *st,
                          uint8_t out[IONIC_LIF_STATS_BYTES]);

/* Render struct ionic_port_stats into @out (little-endian). */
void ionic_emu_port_fill(const struct ionic_emu_stats *st,
                         uint8_t out[IONIC_PORT_STATS_BYTES]);

/* The ionic_if.h name of port counter @idx, or NULL past the end. */
const char *ionic_emu_port_stat_name(unsigned int idx);

/*
 * The ionic_if.h name of the counter at qword @idx ("rx_ucast_bytes"), or
 * NULL for a reserved slot or an index out of range.
 */
const char *ionic_emu_lif_stat_name(unsigned int idx);

#endif /* IONIC_EMU_STATS_H */
