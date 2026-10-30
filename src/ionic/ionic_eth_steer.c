/*
 * Receive steering for the emulated ionic NIC.  See ionic_eth_steer.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "byteorder.h"
#include "ionic/ionic_eth_steer.h"

#define FNV1A_PRIME 0x01000193U

#define ETH_MACS_LEN     12U /* destination and source MAC */
#define ETH_TYPE_LEN     2U
#define VLAN_TAG_LEN     4U
#define VLAN_TAGS_MAX    2U
#define ETHERTYPE_IPV4   0x0800U
#define ETHERTYPE_IPV6   0x86ddU
#define ETHERTYPE_8021Q  0x8100U
#define ETHERTYPE_8021AD 0x88a8U

#define IPV4_HDR_MIN      20U
#define IPV4_FRAG_OFF     6U      /* flags and fragment offset, be16 */
#define IPV4_FRAG_MF_MASK 0x3fffU /* MF and the 13-bit offset */
#define IPV4_PROTO_OFF    9U
#define IPV4_ADDRS_OFF    12U /* source then destination, 4 bytes each */
#define IPV4_ADDRS_LEN    8U

#define IPV6_HDR_LEN   40U
#define IPV6_NH_OFF    6U
#define IPV6_ADDRS_OFF 8U /* source then destination, 16 bytes each */
#define IPV6_ADDRS_LEN 32U

#define PORTS_LEN 4U /* source then destination port */

#define IPPROTO_NUM_TCP  6U
#define IPPROTO_NUM_UDP  17U
#define IPPROTO_NUM_SCTP 132U

static uint32_t fnv1a(uint32_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= FNV1A_PRIME;
    }
    return h;
}

static bool has_ports(uint8_t proto)
{
    return proto == IPPROTO_NUM_TCP || proto == IPPROTO_NUM_UDP ||
           proto == IPPROTO_NUM_SCTP;
}

/*
 * Hash the addresses, the protocol and, when @ports_ok and the frame holds
 * them, the ports at @l4.  @proto points into the frame.
 */
static uint32_t hash_l3(const uint8_t *frame, size_t len, size_t addrs,
                        size_t addrs_len, const uint8_t *proto, size_t l4,
                        bool ports_ok)
{
    uint32_t h = fnv1a(IONIC_ETH_STEER_HASH_EMPTY, frame + addrs, addrs_len);

    h = fnv1a(h, proto, 1);
    if (ports_ok && has_ports(*proto) && l4 <= len && len - l4 >= PORTS_LEN) {
        h = fnv1a(h, frame + l4, PORTS_LEN);
    }
    return h;
}

/* IPv4 at @l3; false if the header doesn't fit, so the caller uses L2. */
static bool hash_ipv4(const uint8_t *frame, size_t len, size_t l3,
                      uint32_t *out)
{
    const uint8_t *ip = frame + l3;
    size_t ihl;

    if (len - l3 < IPV4_HDR_MIN || (ip[0] >> 4) != 4) {
        return false;
    }
    ihl = (size_t)(ip[0] & 0x0fU) * 4U;
    if (ihl < IPV4_HDR_MIN || ihl > len - l3) {
        return false;
    }
    *out =
        hash_l3(frame, len, l3 + IPV4_ADDRS_OFF, IPV4_ADDRS_LEN,
                ip + IPV4_PROTO_OFF, l3 + ihl,
                (ernic_get_be16(ip + IPV4_FRAG_OFF) & IPV4_FRAG_MF_MASK) == 0);
    return true;
}

/* IPv6 at @l3; false if the header doesn't fit. */
static bool hash_ipv6(const uint8_t *frame, size_t len, size_t l3,
                      uint32_t *out)
{
    const uint8_t *ip = frame + l3;

    if (len - l3 < IPV6_HDR_LEN || (ip[0] >> 4) != 6) {
        return false;
    }
    *out = hash_l3(frame, len, l3 + IPV6_ADDRS_OFF, IPV6_ADDRS_LEN,
                   ip + IPV6_NH_OFF, l3 + IPV6_HDR_LEN, true);
    return true;
}

uint32_t ionic_eth_flow_hash(const uint8_t *frame, size_t len)
{
    size_t off = ETH_MACS_LEN;
    uint32_t h;

    if (!frame || len < ETH_MACS_LEN) {
        return IONIC_ETH_STEER_HASH_EMPTY;
    }

    for (unsigned int tags = 0;; tags++) {
        uint16_t type;

        if (len - off < ETH_TYPE_LEN) {
            break;
        }
        type = ernic_get_be16(frame + off);
        off += ETH_TYPE_LEN;
        if ((type == ETHERTYPE_8021Q || type == ETHERTYPE_8021AD) &&
            tags < VLAN_TAGS_MAX) {
            /* The TCI, then the next EtherType. */
            if (len - off < VLAN_TAG_LEN - ETH_TYPE_LEN) {
                break;
            }
            off += VLAN_TAG_LEN - ETH_TYPE_LEN;
            continue;
        }
        if (type == ETHERTYPE_IPV4 && hash_ipv4(frame, len, off, &h)) {
            return h;
        }
        if (type == ETHERTYPE_IPV6 && hash_ipv6(frame, len, off, &h)) {
            return h;
        }
        break;
    }
    return fnv1a(IONIC_ETH_STEER_HASH_EMPTY, frame, ETH_MACS_LEN);
}
