/*
 * Ethernet Tx offloads of the emulated ionic NIC: the checksum a
 * CSUM_PARTIAL descriptor asks for, and the headers of each segment of a
 * TSO descriptor chain.
 *
 * These work on a frame already gathered out of guest memory, so they do
 * no DMA.  Every offset and length they are given is the guest's and is
 * checked against the frame before it is used.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef IONIC_ETH_TX_H
#define IONIC_ETH_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * IONIC_TXQ_DESC_OPCODE_CSUM_PARTIAL, which is the kernel's
 * skb_checksum_help(): sum @frame from @csum_start to its end and store the
 * Internet checksum @csum_offset bytes after @csum_start.  The field is
 * summed with the rest, since the driver left the pseudo-header's sum in
 * it, and a computed zero is stored as 0xffff, which UDP requires and TCP
 * reads as the same value.
 *
 * Returns false, leaving @frame as it was, if the two-byte field does not
 * lie inside the @len bytes of @frame.
 */
bool ionic_eth_tx_csum_partial(uint8_t *frame, size_t len, uint16_t csum_start,
                               uint16_t csum_offset);

/* What a TSO chain's header template says, from ionic_eth_tso_parse(). */
struct ionic_eth_tso {
    uint16_t hdr_len; /* Ethernet through the end of the TCP header */
    uint16_t l3_off;  /* the IPv4 or IPv6 header */
    uint16_t l4_off;  /* the TCP header */
    bool ipv6;
    uint16_t ip_id;    /* IPv4: the template's identification */
    uint32_t seq;      /* the template's sequence number */
    uint8_t tcp_flags; /* the template's TCP flags */
};

/*
 * Parse the @hdr_len bytes of headers a TSO chain's first descriptor starts
 * with: Ethernet, at most one 802.1Q or 802.1ad tag, IPv4 (with options) or
 * IPv6 (with hop-by-hop, destination-options and exhausted routing
 * headers), then TCP, whose header must end exactly at @hdr_len.
 *
 * Returns false for anything else, including a fragment, a SYN or RST, a
 * routing header with segments left (the pseudo-header's destination is
 * then not the IPv6 header's), and any other extension header.
 */
bool ionic_eth_tso_parse(const uint8_t *hdr, size_t hdr_len,
                         struct ionic_eth_tso *t);

/*
 * Turn @frame, @t's header template followed by one segment's payload, @len
 * bytes in all, into the frame for the wire.  @index counts the chain's
 * segments from 0, @payload_off is how many payload bytes the segments
 * before it carried, and @last marks the final one.
 *
 * Writes the IPv4 total length, identification (the template's plus @index)
 * and header checksum, or the IPv6 payload length; the TCP sequence number;
 * clears FIN and PSH except on the last segment and CWR except on the
 * first; and computes the TCP checksum from nothing, pseudo-header
 * included.  What the driver left in the checksum fields is not used: it
 * prepares them only for a frame without a VLAN tag.
 *
 * @len must be more than @t->hdr_len and at most 65535, which the caller's
 * frame buffer guarantees.
 */
void ionic_eth_tso_segment(const struct ionic_eth_tso *t, uint8_t *frame,
                           size_t len, uint32_t index, uint32_t payload_off,
                           bool last);

#endif /* IONIC_ETH_TX_H */
