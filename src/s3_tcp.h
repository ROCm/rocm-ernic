/*
 * s3_tcp.h — the in-band TCP endpoint the S3 control plane listens on
 *
 * The S3 target's control plane has to be reachable from inside the
 * guest, and the guest reaches things through the emulated ionic NIC.
 * So rather than opening a host socket and asking the guest to route out
 * to it, this module terminates TCP on the emulated wire itself: guest
 * TX frames are offered here before they go anywhere else, and the
 * replies are injected back into the guest's RX queue.  From the guest's
 * point of view there is simply an HTTP server at the target address, in
 * the same way the NVMe-oF backend puts a discovery controller at
 * 192.168.200.1:4420.
 *
 * What is implemented is the smallest TCP that a well-behaved local peer
 * on a lossless link needs: ARP so the guest can resolve the address,
 * ICMP echo so it can be pinged, the three-way handshake, in-order data
 * with cumulative ACKs, MSS-sized segmentation, one retransmission timer
 * per connection, and an orderly FIN.  There is no congestion control,
 * no selective ACK, no out-of-order reassembly and no fragmentation:
 * every one of those exists to cope with a real network, and there is
 * not one between these two endpoints.
 *
 * Frames in, frames out through a callback.  Nothing here knows about
 * vfio-user or queues, so the whole stack runs under CTest against a
 * loopback that hands frames straight back.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef S3_TCP_H
#define S3_TCP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "s3_target.h"

struct s3_tcp;

#define S3_TCP_MAX_CONNS 8

struct s3_tcp_cfg {
    uint32_t ip;   /* address the endpoint answers on, host byte order */
    uint16_t port; /* TCP port the S3 control plane listens on */
    uint8_t mac[6];
    unsigned max_conns; /* 0 selects S3_TCP_MAX_CONNS */
};

struct s3_tcp_stats {
    uint64_t frames_rx;
    uint64_t frames_tx;
    uint64_t arp_replies;
    uint64_t pings;
    uint64_t accepts;
    uint64_t requests;
    uint64_t retransmits;
    uint64_t resets;
    uint64_t refused;
};

/* Called with a complete Ethernet frame to hand to the guest. */
typedef void (*s3_tcp_tx_fn)(void *ctx, const void *frame, size_t len);

/*
 * Fill @cfg from @target's configured address and port and a MAC derived
 * from that address, so the common case needs no further setup.
 */
void s3_tcp_cfg_from_target(struct s3_tcp_cfg *cfg,
                            const struct s3_target *target);

struct s3_tcp *s3_tcp_create(const struct s3_tcp_cfg *cfg,
                             struct s3_target *target,
                             const struct s3_dma_ops *dma, void *dma_ctx,
                             s3_tcp_tx_fn tx, void *tx_ctx, char *err,
                             size_t errlen);
void s3_tcp_destroy(struct s3_tcp *s);

/*
 * Offer one guest TX frame to the endpoint.
 *
 * Returns true when the frame was for us and has been consumed, false
 * when the caller should keep looking for somewhere else to put it.
 * @now_ms is a monotonic millisecond clock; the caller owns it so that
 * tests can drive time directly.
 */
bool s3_tcp_rx_frame(struct s3_tcp *s, const void *frame, size_t len,
                     uint64_t now_ms);

/*
 * Push pending output: retransmissions, queued response bytes that would
 * not fit in the peer's window, and connection reaping.  Returns true if
 * anything was sent, so the caller's main loop knows not to sleep.
 */
bool s3_tcp_poll(struct s3_tcp *s, uint64_t now_ms);

/* True when a poll would have something to do right now. */
bool s3_tcp_has_work(const struct s3_tcp *s);

const struct s3_tcp_stats *s3_tcp_statistics(const struct s3_tcp *s);
unsigned s3_tcp_conn_count(const struct s3_tcp *s);

#endif /* S3_TCP_H */
