/*
 * Counters for the emulated ionic NIC's statistics blocks.
 *
 * The ionic driver never asks the device for statistics.  It reads them
 * straight out of DMA memory it allocated: struct ionic_lif_stats sits at
 * byte 320 of the struct ionic_lif_info whose address LIF_INIT passes, and
 * ionic_get_stats64() turns it into what `ip -s link`, /proc/net/dev and
 * IFLA_STATS64 report.  The device has to keep that block current itself.
 *
 * struct ionic_emu_stats is the emulator's copy.  The datapath counts each
 * event once, here; ionic_emu_stats_fill() renders the guest's little-endian
 * block from it, and the --stats-file writer prints the same values, so a
 * test can compare what the guest sees with what the emulator counted.
 *
 * enum ionic_lif_stat is the only place that knows where each counter sits
 * in the block.  Its values are qword indices, copied from struct
 * ionic_lif_stats in the kernel's ionic_if.h; the reserved slots are never
 * written, so they stay zero as the driver expects.
 *
 * Everything here runs on the main loop thread except
 * ionic_emu_stats_rx_inbox_full(), which the mesh receive thread calls.  It
 * counts into atomics that the main thread folds into the block.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef IONIC_EMU_STATS_H
#define IONIC_EMU_STATS_H

#include <stdatomic.h>
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

    /* Mesh inbox overflow, counted off the main thread; see fold(). */
    _Atomic uint64_t inbox_drop_bytes[IONIC_EMU_MAC_CLASSES];
    _Atomic uint64_t inbox_drop_packets[IONIC_EMU_MAC_CLASSES];
};

/*
 * The class of a frame by its destination MAC, or -1 if @len is too short
 * to hold one.
 */
int ionic_emu_mac_class(const uint8_t *frame, size_t len);

/* Zero every counter, including the atomics. */
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

/* The mesh inbox was full.  Safe to call from any thread. */
void ionic_emu_stats_rx_inbox_full(struct ionic_emu_stats *st,
                                   const uint8_t *frame, size_t len);

/* One RDMA message of @bytes moved by the RDMA datapath. */
void ionic_emu_stats_rdma(struct ionic_emu_stats *st, uint64_t bytes,
                          bool is_tx);

/* Move the inbox atomics into the block, zeroing them.  Main thread only. */
void ionic_emu_stats_fold(struct ionic_emu_stats *st);

/*
 * Fold, then render struct ionic_lif_stats into @out (little-endian, every
 * reserved qword zero).
 */
void ionic_emu_stats_fill(struct ionic_emu_stats *st,
                          uint8_t out[IONIC_LIF_STATS_BYTES]);

/*
 * The ionic_if.h name of the counter at qword @idx ("rx_ucast_bytes"), or
 * NULL for a reserved slot or an index out of range.
 */
const char *ionic_emu_lif_stat_name(unsigned int idx);

#endif /* IONIC_EMU_STATS_H */
