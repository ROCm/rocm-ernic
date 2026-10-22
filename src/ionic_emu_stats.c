/*
 * Counters for the emulated ionic NIC's statistics blocks.
 *
 * See ionic_emu_stats.h for what reads them and which thread may call what.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "ionic_emu_stats.h"

#include <endian.h>
#include <string.h>

/* The first counter of each per-class pair, indexed by ionic_emu_mac_class. */
static const unsigned int rx_bytes_idx[IONIC_EMU_MAC_CLASSES] = {
    IONIC_LIF_RX_UCAST_BYTES, IONIC_LIF_RX_MCAST_BYTES,
    IONIC_LIF_RX_BCAST_BYTES};
static const unsigned int tx_bytes_idx[IONIC_EMU_MAC_CLASSES] = {
    IONIC_LIF_TX_UCAST_BYTES, IONIC_LIF_TX_MCAST_BYTES,
    IONIC_LIF_TX_BCAST_BYTES};
static const unsigned int rx_drop_bytes_idx[IONIC_EMU_MAC_CLASSES] = {
    IONIC_LIF_RX_UCAST_DROP_BYTES, IONIC_LIF_RX_MCAST_DROP_BYTES,
    IONIC_LIF_RX_BCAST_DROP_BYTES};
static const unsigned int tx_drop_bytes_idx[IONIC_EMU_MAC_CLASSES] = {
    IONIC_LIF_TX_UCAST_DROP_BYTES, IONIC_LIF_TX_MCAST_DROP_BYTES,
    IONIC_LIF_TX_BCAST_DROP_BYTES};

/* In each pair the packet counter follows the byte counter. */
#define PACKETS_OF(bytes_idx) ((bytes_idx) + 1u)

int ionic_emu_mac_class(const uint8_t *frame, size_t len)
{
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    if (!frame || len < 6)
        return -1;
    if (memcmp(frame, bcast, sizeof(bcast)) == 0)
        return IONIC_EMU_BCAST;
    return (frame[0] & 0x01) ? IONIC_EMU_MCAST : IONIC_EMU_UCAST;
}

void ionic_emu_stats_reset(struct ionic_emu_stats *st)
{
    memset(st->lif, 0, sizeof(st->lif));
    for (unsigned int c = 0; c < IONIC_EMU_MAC_CLASSES; c++) {
        atomic_store(&st->inbox_drop_bytes[c], 0);
        atomic_store(&st->inbox_drop_packets[c], 0);
    }
}

static void add_pair(struct ionic_emu_stats *st, unsigned int bytes_idx,
                     uint64_t bytes)
{
    st->lif[bytes_idx] += bytes;
    st->lif[PACKETS_OF(bytes_idx)]++;
}

void ionic_emu_stats_frame(struct ionic_emu_stats *st, const uint8_t *frame,
                           size_t len, bool is_tx)
{
    int c;

    if (len == 0)
        return;
    c = ionic_emu_mac_class(frame, len);
    if (c < 0) {
        /* A runt with no destination MAC: a malformed descriptor. */
        st->lif[is_tx ? IONIC_LIF_TX_DESC_DATA_ERROR
                      : IONIC_LIF_RX_DESC_DATA_ERROR]++;
        return;
    }
    add_pair(st, is_tx ? tx_bytes_idx[c] : rx_bytes_idx[c], len);
}

/* The queue/ring counter for @why, or -1 when the class counters say it all. */
static int drop_reason_idx(bool is_tx, enum ionic_emu_drop why)
{
    switch (why) {
    case IONIC_EMU_DROP_NO_BUFFER:
        return is_tx ? IONIC_LIF_TX_QUEUE_EMPTY : IONIC_LIF_RX_QUEUE_EMPTY;
    case IONIC_EMU_DROP_TOO_BIG:
    case IONIC_EMU_DROP_DESC_DATA:
        return is_tx ? IONIC_LIF_TX_DESC_DATA_ERROR
                     : IONIC_LIF_RX_DESC_DATA_ERROR;
    case IONIC_EMU_DROP_DESC_FETCH:
        return is_tx ? IONIC_LIF_TX_DESC_FETCH_ERROR
                     : IONIC_LIF_RX_DESC_FETCH_ERROR;
    case IONIC_EMU_DROP_DMA:
        return is_tx ? IONIC_LIF_TX_DMA_ERROR : IONIC_LIF_RX_DMA_ERROR;
    case IONIC_EMU_DROP_DISABLED:
        return is_tx ? IONIC_LIF_TX_QUEUE_DISABLED
                     : IONIC_LIF_RX_QUEUE_DISABLED;
    case IONIC_EMU_DROP_SEND:
    case IONIC_EMU_DROP_INJECTED:
    default:
        return -1;
    }
}

void ionic_emu_stats_drop(struct ionic_emu_stats *st, const uint8_t *frame,
                          size_t len, bool is_tx, enum ionic_emu_drop why)
{
    int reason = drop_reason_idx(is_tx, why);
    int c = ionic_emu_mac_class(frame, len);

    if (reason >= 0)
        st->lif[reason]++;
    if (c >= 0)
        add_pair(st, is_tx ? tx_drop_bytes_idx[c] : rx_drop_bytes_idx[c], len);
}

void ionic_emu_stats_rx_inbox_full(struct ionic_emu_stats *st,
                                   const uint8_t *frame, size_t len)
{
    int c = ionic_emu_mac_class(frame, len);

    if (c < 0)
        return;
    atomic_fetch_add(&st->inbox_drop_bytes[c], (uint64_t)len);
    atomic_fetch_add(&st->inbox_drop_packets[c], 1);
}

void ionic_emu_stats_rdma(struct ionic_emu_stats *st, uint64_t bytes,
                          bool is_tx)
{
    if (bytes == 0)
        return;
    add_pair(st,
             is_tx ? IONIC_LIF_TX_RDMA_UCAST_BYTES
                   : IONIC_LIF_RX_RDMA_UCAST_BYTES,
             bytes);
}

void ionic_emu_stats_fold(struct ionic_emu_stats *st)
{
    for (unsigned int c = 0; c < IONIC_EMU_MAC_CLASSES; c++) {
        unsigned int idx = rx_drop_bytes_idx[c];

        st->lif[idx] += atomic_exchange(&st->inbox_drop_bytes[c], 0);
        st->lif[PACKETS_OF(idx)] +=
            atomic_exchange(&st->inbox_drop_packets[c], 0);
    }
}

void ionic_emu_stats_fill(struct ionic_emu_stats *st,
                          uint8_t out[IONIC_LIF_STATS_BYTES])
{
    ionic_emu_stats_fold(st);
    memset(out, 0, IONIC_LIF_STATS_BYTES);
    for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++) {
        uint64_t v;

        /* Only named counters reach the guest; reserved slots stay zero. */
        if (!ionic_emu_lif_stat_name(q))
            continue;
        v = htole64(st->lif[q]);
        memcpy(out + (size_t)q * 8u, &v, sizeof(v));
    }
}

/* Names as in struct ionic_lif_stats; NULL marks a reserved qword. */
static const char *const lif_stat_names[IONIC_LIF_STATS_QWORDS] = {
    [IONIC_LIF_RX_UCAST_BYTES] = "rx_ucast_bytes",
    [IONIC_LIF_RX_UCAST_PACKETS] = "rx_ucast_packets",
    [IONIC_LIF_RX_MCAST_BYTES] = "rx_mcast_bytes",
    [IONIC_LIF_RX_MCAST_PACKETS] = "rx_mcast_packets",
    [IONIC_LIF_RX_BCAST_BYTES] = "rx_bcast_bytes",
    [IONIC_LIF_RX_BCAST_PACKETS] = "rx_bcast_packets",
    [IONIC_LIF_RX_UCAST_DROP_BYTES] = "rx_ucast_drop_bytes",
    [IONIC_LIF_RX_UCAST_DROP_PACKETS] = "rx_ucast_drop_packets",
    [IONIC_LIF_RX_MCAST_DROP_BYTES] = "rx_mcast_drop_bytes",
    [IONIC_LIF_RX_MCAST_DROP_PACKETS] = "rx_mcast_drop_packets",
    [IONIC_LIF_RX_BCAST_DROP_BYTES] = "rx_bcast_drop_bytes",
    [IONIC_LIF_RX_BCAST_DROP_PACKETS] = "rx_bcast_drop_packets",
    [IONIC_LIF_RX_DMA_ERROR] = "rx_dma_error",
    [IONIC_LIF_TX_UCAST_BYTES] = "tx_ucast_bytes",
    [IONIC_LIF_TX_UCAST_PACKETS] = "tx_ucast_packets",
    [IONIC_LIF_TX_MCAST_BYTES] = "tx_mcast_bytes",
    [IONIC_LIF_TX_MCAST_PACKETS] = "tx_mcast_packets",
    [IONIC_LIF_TX_BCAST_BYTES] = "tx_bcast_bytes",
    [IONIC_LIF_TX_BCAST_PACKETS] = "tx_bcast_packets",
    [IONIC_LIF_TX_UCAST_DROP_BYTES] = "tx_ucast_drop_bytes",
    [IONIC_LIF_TX_UCAST_DROP_PACKETS] = "tx_ucast_drop_packets",
    [IONIC_LIF_TX_MCAST_DROP_BYTES] = "tx_mcast_drop_bytes",
    [IONIC_LIF_TX_MCAST_DROP_PACKETS] = "tx_mcast_drop_packets",
    [IONIC_LIF_TX_BCAST_DROP_BYTES] = "tx_bcast_drop_bytes",
    [IONIC_LIF_TX_BCAST_DROP_PACKETS] = "tx_bcast_drop_packets",
    [IONIC_LIF_TX_DMA_ERROR] = "tx_dma_error",
    [IONIC_LIF_RX_QUEUE_DISABLED] = "rx_queue_disabled",
    [IONIC_LIF_RX_QUEUE_EMPTY] = "rx_queue_empty",
    [IONIC_LIF_RX_QUEUE_ERROR] = "rx_queue_error",
    [IONIC_LIF_RX_DESC_FETCH_ERROR] = "rx_desc_fetch_error",
    [IONIC_LIF_RX_DESC_DATA_ERROR] = "rx_desc_data_error",
    [IONIC_LIF_TX_QUEUE_DISABLED] = "tx_queue_disabled",
    [IONIC_LIF_TX_QUEUE_ERROR] = "tx_queue_error",
    [IONIC_LIF_TX_DESC_FETCH_ERROR] = "tx_desc_fetch_error",
    [IONIC_LIF_TX_DESC_DATA_ERROR] = "tx_desc_data_error",
    [IONIC_LIF_TX_QUEUE_EMPTY] = "tx_queue_empty",
    [IONIC_LIF_TX_RDMA_UCAST_BYTES] = "tx_rdma_ucast_bytes",
    [IONIC_LIF_TX_RDMA_UCAST_PACKETS] = "tx_rdma_ucast_packets",
    [IONIC_LIF_TX_RDMA_MCAST_BYTES] = "tx_rdma_mcast_bytes",
    [IONIC_LIF_TX_RDMA_MCAST_PACKETS] = "tx_rdma_mcast_packets",
    [IONIC_LIF_TX_RDMA_CNP_PACKETS] = "tx_rdma_cnp_packets",
    [IONIC_LIF_RX_RDMA_UCAST_BYTES] = "rx_rdma_ucast_bytes",
    [IONIC_LIF_RX_RDMA_UCAST_PACKETS] = "rx_rdma_ucast_packets",
    [IONIC_LIF_RX_RDMA_MCAST_BYTES] = "rx_rdma_mcast_bytes",
    [IONIC_LIF_RX_RDMA_MCAST_PACKETS] = "rx_rdma_mcast_packets",
    [IONIC_LIF_RX_RDMA_CNP_PACKETS] = "rx_rdma_cnp_packets",
    [IONIC_LIF_RX_RDMA_ECN_PACKETS] = "rx_rdma_ecn_packets",
    [IONIC_LIF_RDMA_REQ_RX_PKT_SEQ_ERR] = "rdma_req_rx_pkt_seq_err",
    [IONIC_LIF_RDMA_REQ_RX_RNR_RETRY_ERR] = "rdma_req_rx_rnr_retry_err",
    [IONIC_LIF_RDMA_REQ_RX_REMOTE_ACCESS_ERR] = "rdma_req_rx_remote_access_err",
    [IONIC_LIF_RDMA_REQ_RX_REMOTE_INV_REQ_ERR] =
        "rdma_req_rx_remote_inv_req_err",
    [IONIC_LIF_RDMA_REQ_RX_REMOTE_OPER_ERR] = "rdma_req_rx_remote_oper_err",
    [IONIC_LIF_RDMA_REQ_RX_IMPLIED_NAK_SEQ_ERR] =
        "rdma_req_rx_implied_nak_seq_err",
    [IONIC_LIF_RDMA_REQ_RX_CQE_ERR] = "rdma_req_rx_cqe_err",
    [IONIC_LIF_RDMA_REQ_RX_CQE_FLUSH_ERR] = "rdma_req_rx_cqe_flush_err",
    [IONIC_LIF_RDMA_REQ_RX_DUP_RESPONSES] = "rdma_req_rx_dup_responses",
    [IONIC_LIF_RDMA_REQ_RX_INVALID_PACKETS] = "rdma_req_rx_invalid_packets",
    [IONIC_LIF_RDMA_REQ_TX_LOCAL_ACCESS_ERR] = "rdma_req_tx_local_access_err",
    [IONIC_LIF_RDMA_REQ_TX_LOCAL_OPER_ERR] = "rdma_req_tx_local_oper_err",
    [IONIC_LIF_RDMA_REQ_TX_MEMORY_MGMT_ERR] = "rdma_req_tx_memory_mgmt_err",
    [IONIC_LIF_RDMA_RESP_RX_DUP_REQUESTS] = "rdma_resp_rx_dup_requests",
    [IONIC_LIF_RDMA_RESP_RX_OUT_OF_BUFFER] = "rdma_resp_rx_out_of_buffer",
    [IONIC_LIF_RDMA_RESP_RX_OUT_OF_SEQ_PKTS] = "rdma_resp_rx_out_of_seq_pkts",
    [IONIC_LIF_RDMA_RESP_RX_CQE_ERR] = "rdma_resp_rx_cqe_err",
    [IONIC_LIF_RDMA_RESP_RX_CQE_FLUSH_ERR] = "rdma_resp_rx_cqe_flush_err",
    [IONIC_LIF_RDMA_RESP_RX_LOCAL_LEN_ERR] = "rdma_resp_rx_local_len_err",
    [IONIC_LIF_RDMA_RESP_RX_INV_REQUEST_ERR] = "rdma_resp_rx_inv_request_err",
    [IONIC_LIF_RDMA_RESP_RX_LOCAL_QP_OPER_ERR] =
        "rdma_resp_rx_local_qp_oper_err",
    [IONIC_LIF_RDMA_RESP_RX_OUT_OF_ATOMIC_RESOURCE] =
        "rdma_resp_rx_out_of_atomic_resource",
    [IONIC_LIF_RDMA_RESP_TX_PKT_SEQ_ERR] = "rdma_resp_tx_pkt_seq_err",
    [IONIC_LIF_RDMA_RESP_TX_REMOTE_INV_REQ_ERR] =
        "rdma_resp_tx_remote_inv_req_err",
    [IONIC_LIF_RDMA_RESP_TX_REMOTE_ACCESS_ERR] =
        "rdma_resp_tx_remote_access_err",
    [IONIC_LIF_RDMA_RESP_TX_REMOTE_OPER_ERR] = "rdma_resp_tx_remote_oper_err",
    [IONIC_LIF_RDMA_RESP_TX_RNR_RETRY_ERR] = "rdma_resp_tx_rnr_retry_err",
};

const char *ionic_emu_lif_stat_name(unsigned int idx)
{
    return idx < IONIC_LIF_STATS_QWORDS ? lif_stat_names[idx] : NULL;
}
