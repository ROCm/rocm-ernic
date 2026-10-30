/*
 * Receive steering for the emulated ionic NIC: which Rx queue a frame
 * lands on.
 *
 * The hash is fixed and simple so that a test can predict the queue: 32-bit
 * FNV-1a over the flow's identity, taken modulo the number of Rx queues the
 * guest has enabled.  The guest's RSS key and indirection table are ignored,
 * as the emulator never honored them.
 *
 * The identity, in this order, all in network byte order as on the wire:
 *
 *   IPv4: source and destination address, protocol, and for TCP, UDP and
 *         SCTP the source and destination port.  A fragment (MF set or a
 *         non-zero offset) carries no ports past the first, so no fragment
 *         uses them, and every fragment of a datagram lands together.
 *   IPv6: source and destination address, next header, and the ports as
 *         for IPv4 when the next header is TCP, UDP or SCTP.  Extension
 *         headers are not walked; a fragment header (44) hashes without
 *         ports for the same reason as above.
 *   else: the destination and source MAC (bytes 0..11): ARP, any other
 *         EtherType, and any IP frame too short for the header it claims.
 *
 * Up to two VLAN tags (802.1Q 0x8100, 802.1ad 0x88a8) are skipped before
 * the EtherType.  Frames shorter than the two MACs hash as empty input.
 * Nothing past @len is ever read.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef IONIC_ETH_STEER_H
#define IONIC_ETH_STEER_H

#include <stddef.h>
#include <stdint.h>

/* The FNV-1a 32-bit offset basis: the hash of empty input. */
#define IONIC_ETH_STEER_HASH_EMPTY 0x811c9dc5U

/* The flow hash of @frame's first @len bytes, as described above. */
uint32_t ionic_eth_flow_hash(const uint8_t *frame, size_t len);

#endif /* IONIC_ETH_STEER_H */
