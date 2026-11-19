/*
 * Ethernet Rx offloads of the emulated ionic NIC (ionic_eth_rx.h).
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "ionic/ionic_eth_rx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "byteorder.h"
#include "net/net_headers.h"

/* Bytes in a word of an IPv4 or TCP header length field */
#define HDR_LEN_UNIT 4U

/* The VLAN tags a frame may have in front of its IP header */
#define RX_VLAN_TAGS_MAX 2U

/* What follows a frame's IP header, as far as the checksums need it. */
struct rx_l4 {
    size_t off;       /* where it starts in the frame */
    size_t len;       /* its length, from the IP header: inside the frame */
    uint8_t protocol; /* IP_PROTOCOL_* */
    uint32_t pseudo;  /* the sum of the pseudo-header's two addresses */
};

static bool is_vlan_ethertype(uint16_t ethertype)
{
    return ethertype == ETH_ETHERTYPE_VLAN || ethertype == ETH_ETHERTYPE_QINQ;
}

/*
 * The IPv4 header at @l3 of @frame.  Sets IP_OK or IP_BAD in *@flags if
 * there is one, and returns true with @l4 filled in if it is good, not a
 * fragment, and its datagram lies inside the frame.
 */
static bool rx_ipv4(const uint8_t *frame, size_t len, size_t l3, uint8_t *flags,
                    struct rx_l4 *l4)
{
    if (len - l3 < IP_HDR_LEN) {
        return false;
    }

    const uint8_t *ip = frame + l3;
    size_t ihl = (size_t)(ip[IP_VERSION_IHL_OFF] & 0xfU) * HDR_LEN_UNIT;

    if (ip[IP_VERSION_IHL_OFF] >> IP_VERSION_SHIFT != IP_VERSION ||
        ihl < IP_HDR_LEN || ihl > len - l3) {
        return false;
    }
    if (net_csum_fold(net_csum_add(0, ip, ihl)) != 0) {
        *flags |= IONIC_RXQ_CSUM_F_IP_BAD;
        return false;
    }
    *flags |= IONIC_RXQ_CSUM_F_IP_OK;

    size_t total = ernic_get_be16(ip + IP_TOTAL_LEN_OFF);

    if ((ernic_get_be16(ip + IP_FRAGMENT_OFF) & IP_FRAGMENT_MASK) != 0 ||
        total < ihl || total > len - l3) {
        return false;
    }
    l4->off = l3 + ihl;
    l4->len = total - ihl;
    l4->protocol = ip[IP_PROTOCOL_OFF];
    l4->pseudo = net_csum_add(0, ip + IP_SRC_OFF, 2U * sizeof(uint32_t));
    return true;
}

/*
 * The same for an IPv6 header, which has no checksum.  Extension headers
 * are not walked: what the fixed header names next is the protocol.
 */
static bool rx_ipv6(const uint8_t *frame, size_t len, size_t l3,
                    struct rx_l4 *l4)
{
    if (len - l3 < IP6_HDR_LEN) {
        return false;
    }

    const uint8_t *ip = frame + l3;
    size_t payload = ernic_get_be16(ip + IP6_PAYLOAD_OFF);

    if (ip[0] >> IP6_VERSION_SHIFT != IP6_VERSION ||
        payload > len - l3 - IP6_HDR_LEN) {
        return false;
    }
    l4->off = l3 + IP6_HDR_LEN;
    l4->len = payload;
    l4->protocol = ip[IP6_NEXT_HDR_OFF];
    l4->pseudo = net_csum_add(0, ip + IP6_SRC_OFF, (size_t)2 * IP6_ADDR_LEN);
    return true;
}

/*
 * Whether the Internet checksum over @l4 and its pseudo-header is right.
 * The checksum field is summed with the rest, so a good one gives a sum of
 * 0xffff, which folds to zero.
 */
static bool rx_l4_csum_ok(const uint8_t *frame, const struct rx_l4 *l4)
{
    /* The pseudo-header: both addresses, summed already, then the protocol
     * and the length, each as a word of its own.  @l4->len fits 16 bits. */
    uint32_t sum = l4->pseudo + l4->protocol + (uint32_t)l4->len;

    return net_csum_fold(net_csum_add(sum, frame + l4->off, l4->len)) == 0;
}

void ionic_eth_rx_parse(const uint8_t *frame, size_t len,
                        struct ionic_eth_rx_info *info)
{
    memset(info, 0, sizeof(*info));
    if (len < ETH_HDR_LEN) {
        return;
    }

    /* net_csum_add() returns the sum already folded to 16 bits. */
    info->csum =
        (uint16_t)net_csum_add(0, frame + ETH_HDR_LEN, len - ETH_HDR_LEN);
    info->csum_flags = IONIC_RXQ_CSUM_F_CALC;

    /* The ethertype is the two bytes before @l3; each tag moves both on. */
    size_t l3 = ETH_HDR_LEN;
    uint16_t ethertype = ernic_get_be16(frame + ETH_TYPE_OFF);

    for (unsigned int tags = 0;
         tags < RX_VLAN_TAGS_MAX && is_vlan_ethertype(ethertype); tags++) {
        if (len - l3 < ETH_VLAN_TAG_LEN) {
            return;
        }
        l3 += ETH_VLAN_TAG_LEN;
        ethertype = ernic_get_be16(frame + l3 - sizeof(uint16_t));
    }

    struct rx_l4 l4;
    bool ipv4 = ethertype == ETH_ETHERTYPE_IP;

    /* A third tag is neither IP ethertype, and stops here. */
    if (ipv4) {
        if (!rx_ipv4(frame, len, l3, &info->csum_flags, &l4)) {
            return;
        }
    } else if (ethertype != ETH_ETHERTYPE_IPV6 ||
               !rx_ipv6(frame, len, l3, &l4)) {
        return;
    }

    const uint8_t *hdr = frame + l4.off;

    if (l4.protocol == IP_PROTOCOL_UDP) {
        /* A UDP length that disagrees with the IP header's leaves the
         * datagram for the stack to judge. */
        if (l4.len >= UDP_HDR_LEN &&
            ernic_get_be16(hdr + UDP_LEN_OFF) == l4.len &&
            ernic_get_be16(hdr + UDP_CHECKSUM_OFF) != 0) {
            info->csum_flags |= rx_l4_csum_ok(frame, &l4)
                                    ? IONIC_RXQ_CSUM_F_UDP_OK
                                    : IONIC_RXQ_CSUM_F_UDP_BAD;
        }
        return;
    }
    if (l4.protocol != IP_PROTOCOL_TCP || l4.len < TCP_HDR_LEN) {
        return;
    }
    if (!rx_l4_csum_ok(frame, &l4)) {
        info->csum_flags |= IONIC_RXQ_CSUM_F_TCP_BAD;
        return;
    }
    info->csum_flags |= IONIC_RXQ_CSUM_F_TCP_OK;

    size_t tcp_hdr_len =
        (size_t)(hdr[TCP_DATA_OFFSET_OFF] >> TCP_DATA_OFFSET_SHIFT) *
        HDR_LEN_UNIT;

    /* Anything after the segment, such as the padding of a short frame,
     * would land in a payload buffer as if it were data. */
    if (l4.off + l4.len != len || tcp_hdr_len < TCP_HDR_LEN ||
        tcp_hdr_len > l4.len) {
        return;
    }
    info->splittable = true;
    /* At most two tags, 60 bytes of IPv4 header or 40 of IPv6, and 60 of
     * TCP header: it fits with room to spare. */
    info->hdr_len = (uint16_t)(l4.off + tcp_hdr_len);
}
