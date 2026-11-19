/*
 * Ethernet Rx offloads of the emulated ionic NIC in header-split mode
 * ([rx] header_split = on): the checksum status a completion reports for a
 * received frame, and where the frame's headers end, if it is one whose
 * headers and payload the device puts in separate buffers.
 *
 * These work on a frame already taken from the wire, so they do no DMA.
 * The frame comes from the host network or another guest: every length in
 * it is checked against the frame before it is used.
 *
 * Header split is not an interface of any real ionic firmware.  The queue
 * feature bit, the completion extension and its flags were made up so that
 * a prototype driver has a device to run against.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef IONIC_ETH_RX_H
#define IONIC_ETH_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* struct ionic_rxq_comp csum_flags (IONIC_RXQ_COMP_CSUM_F_*) */
#define IONIC_RXQ_CSUM_F_TCP_OK  0x01U
#define IONIC_RXQ_CSUM_F_TCP_BAD 0x02U
#define IONIC_RXQ_CSUM_F_UDP_OK  0x04U
#define IONIC_RXQ_CSUM_F_UDP_BAD 0x08U
#define IONIC_RXQ_CSUM_F_IP_OK   0x10U
#define IONIC_RXQ_CSUM_F_IP_BAD  0x20U
#define IONIC_RXQ_CSUM_F_CALC    0x80U

/* What a received frame's completion says about it. */
struct ionic_eth_rx_info {
    /*
     * The ones-complement sum, not inverted, of the frame from the end of
     * its Ethernet header (byte 14) to its last byte, as big-endian 16-bit
     * words: VLAN tags and padding included.  The completion stores it
     * high byte first.
     */
    uint16_t csum;
    uint8_t csum_flags; /* IONIC_RXQ_CSUM_F_* */
    /*
     * The frame is TCP over IPv4 or IPv6 with nothing after the segment,
     * its IPv4 header and TCP checksums are good, and @hdr_len is where the
     * TCP header ends.
     */
    bool splittable;
    uint16_t hdr_len; /* Ethernet through the TCP header; 0 if not splittable */
};

/*
 * Work out @info for the @len bytes of @frame, which has no FCS.
 *
 * A frame shorter than an Ethernet header reports nothing.  Any other has
 * CALC and the sum.  An IPv4 header behind at most two VLAN tags adds IP_OK
 * or IP_BAD.  TCP or UDP directly behind a good, unfragmented IPv4 header
 * or an IPv6 header with no extension header, and wholly inside the frame,
 * adds its _OK or _BAD, checked over the length the IP header gives, so
 * that padding after it does not count; a UDP checksum of zero, which
 * means none was computed, adds neither.
 *
 * The frame is splittable when it has TCP_OK, the IP header's length ends
 * exactly at @len (a padded frame is not split) and the TCP header length
 * is at least 20 bytes and inside the segment.
 */
void ionic_eth_rx_parse(const uint8_t *frame, size_t len,
                        struct ionic_eth_rx_info *info);

#endif /* IONIC_ETH_RX_H */
