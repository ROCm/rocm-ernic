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

#include "ionic/ionic_emu_stats.h"

#include <string.h>

#include "byteorder.h"

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
    memset(st->port, 0, sizeof(st->port));
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
                                   const uint8_t *frame, size_t len,
                                   size_t max_frame)
{
    int c = ionic_emu_mac_class(frame, len);

    if (c < 0)
        return;
    if (!ionic_emu_port_frame(st, frame, len, false, max_frame))
        return;
    ionic_emu_port_rx_dropped(st);
    add_pair(st, rx_drop_bytes_idx[c], len);
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

void ionic_emu_port_clear(struct ionic_emu_stats *st)
{
    memset(st->port, 0, sizeof(st->port));
}

/* ---- Port (MAC) counters ---------------------------------------------- */

/*
 * The largest wire length, FCS included, in each size bucket but the last;
 * frames_{rx,tx}_less_than_64b through _other are consecutive qwords.
 */
static const size_t bucket_max[] = {63,   64,   127,  255,  511, 1023,
                                    1518, 2047, 4095, 8191, 9215};
#define SIZE_BUCKETS (sizeof(bucket_max) / sizeof(bucket_max[0]) + 1u)
#define RX_BUCKETS \
    (IONIC_PORT_FRAMES_RX_OTHER - IONIC_PORT_FRAMES_RX_LESS_THAN_64B + 1)
#define TX_BUCKETS \
    (IONIC_PORT_FRAMES_TX_OTHER - IONIC_PORT_FRAMES_TX_LESS_THAN_64B + 1)
_Static_assert(RX_BUCKETS == SIZE_BUCKETS, "one rx qword per size bucket");
_Static_assert(TX_BUCKETS == SIZE_BUCKETS, "one tx qword per size bucket");

static unsigned int size_bucket(size_t wire)
{
    unsigned int b = 0;

    while (b < SIZE_BUCKETS - 1u && wire > bucket_max[b])
        b++;
    return b;
}

/* The PCP of the outer 802.1Q or 802.1ad tag, or -1 if there is none. */
static int frame_pcp(const uint8_t *frame, size_t len)
{
    uint16_t tpid;

    if (!frame || len < 16)
        return -1;
    tpid = ernic_get_be16(frame + 12);
    if (tpid != 0x8100 && tpid != 0x88a8)
        return -1;
    return frame[14] >> 5;
}

/* The unicast/multicast/broadcast qwords follow the class order. */
_Static_assert(
    (int)IONIC_PORT_FRAMES_RX_MULTICAST ==
            (int)IONIC_PORT_FRAMES_RX_UNICAST + (int)IONIC_EMU_MCAST &&
        (int)IONIC_PORT_FRAMES_RX_BROADCAST ==
            (int)IONIC_PORT_FRAMES_RX_UNICAST + (int)IONIC_EMU_BCAST &&
        (int)IONIC_PORT_FRAMES_TX_MULTICAST ==
            (int)IONIC_PORT_FRAMES_TX_UNICAST + (int)IONIC_EMU_MCAST &&
        (int)IONIC_PORT_FRAMES_TX_BROADCAST ==
            (int)IONIC_PORT_FRAMES_TX_UNICAST + (int)IONIC_EMU_BCAST,
    "class qwords in ionic_emu_mac_class order");

bool ionic_emu_port_frame(struct ionic_emu_stats *st, const uint8_t *frame,
                          size_t len, bool is_tx, size_t max_frame)
{
    uint64_t *port = st->port;
    size_t wire;
    int c, pcp;
    unsigned int pri;

    if (len == 0)
        return true;
    wire = (len < IONIC_EMU_MIN_FRAME ? IONIC_EMU_MIN_FRAME : len) +
           IONIC_EMU_FCS_LEN;
    c = ionic_emu_mac_class(frame, len);
    pcp = frame_pcp(frame, len);
    pri = pcp < 0 ? 0u : (unsigned int)pcp;

    if (is_tx) {
        port[IONIC_PORT_FRAMES_TX_ALL]++;
        port[IONIC_PORT_OCTETS_TX_TOTAL] += wire;
        port[IONIC_PORT_FRAMES_TX_LESS_THAN_64B + size_bucket(wire)]++;
        port[IONIC_PORT_FRAMES_TX_OK]++;
        port[IONIC_PORT_OCTETS_TX_OK] += wire;
        if (c >= 0)
            port[IONIC_PORT_FRAMES_TX_UNICAST + (unsigned int)c]++;
        if (pcp >= 0)
            port[IONIC_PORT_FRAMES_TX_VLAN]++;
        port[IONIC_PORT_FRAMES_TX_PRI_0 + pri]++;
        return true;
    }

    port[IONIC_PORT_FRAMES_RX_ALL]++;
    port[IONIC_PORT_OCTETS_RX_ALL] += wire;
    port[IONIC_PORT_FRAMES_RX_LESS_THAN_64B + size_bucket(wire)]++;
    if (max_frame && len > max_frame) {
        port[IONIC_PORT_FRAMES_RX_TOO_LONG]++;
        port[IONIC_PORT_FRAMES_RX_OVERSIZED]++;
        port[IONIC_PORT_FRAMES_RX_BAD_ALL]++;
        return false;
    }
    port[IONIC_PORT_FRAMES_RX_OK]++;
    port[IONIC_PORT_OCTETS_RX_OK] += wire;
    if (c >= 0)
        port[IONIC_PORT_FRAMES_RX_UNICAST + (unsigned int)c]++;
    if (pcp >= 0)
        port[IONIC_PORT_FRAMES_RX_VLAN_GOOD]++;
    port[IONIC_PORT_FRAMES_RX_PRI_0 + pri]++;
    return true;
}

void ionic_emu_port_rx_dropped(struct ionic_emu_stats *st)
{
    st->port[IONIC_PORT_FRAMES_RX_DROPPED]++;
}

void ionic_emu_port_fill(const struct ionic_emu_stats *st,
                         uint8_t out[IONIC_PORT_STATS_BYTES])
{
    for (unsigned int q = 0; q < IONIC_PORT_STATS_QWORDS; q++)
        ernic_put_le64(out + (size_t)q * 8u, st->port[q]);
}

static const char *const port_stat_names[IONIC_PORT_STATS_QWORDS] = {
#define IONIC_PORT_STAT_NAME(id, name) [IONIC_PORT_##id] = #name,
    IONIC_PORT_STATS(IONIC_PORT_STAT_NAME)
#undef IONIC_PORT_STAT_NAME
};

const char *ionic_emu_port_stat_name(unsigned int idx)
{
    return idx < IONIC_PORT_STATS_QWORDS ? port_stat_names[idx] : NULL;
}

void ionic_emu_stats_fill(const struct ionic_emu_stats *st,
                          uint8_t out[IONIC_LIF_STATS_BYTES])
{
    memset(out, 0, IONIC_LIF_STATS_BYTES);
    for (unsigned int q = 0; q < IONIC_LIF_STATS_QWORDS; q++) {
        /* Only named counters reach the guest; reserved slots stay zero. */
        if (!ionic_emu_lif_stat_name(q))
            continue;
        ernic_put_le64(out + (size_t)q * 8u, st->lif[q]);
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
