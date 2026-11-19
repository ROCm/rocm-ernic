/*
 * Ethernet Tx offloads of the emulated ionic NIC (ionic_eth_tx.h).
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "ionic/ionic_eth_tx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "byteorder.h"
#include "net/net_headers.h"

/* Bytes in a word of an IPv4 or TCP header length field */
#define HDR_LEN_UNIT 4U

bool ionic_eth_tx_csum_partial(uint8_t *frame, size_t len, uint16_t csum_start,
                               uint16_t csum_offset)
{
    /* Two 16-bit values, so the sum can't wrap. */
    size_t field = (size_t)csum_start + csum_offset;

    if (field > len || len - field < sizeof(uint16_t)) {
        return false;
    }

    uint16_t csum =
        net_csum_fold(net_csum_add(0, frame + csum_start, len - csum_start));
    if (csum == 0) {
        csum = 0xffff;
    }
    ernic_put_be16(frame + field, csum);
    return true;
}

static bool is_vlan_ethertype(uint16_t ethertype)
{
    return ethertype == ETH_ETHERTYPE_VLAN || ethertype == ETH_ETHERTYPE_QINQ;
}

/*
 * The offset of the TCP header behind the IPv4 header at @l3_off, or 0 if
 * it is not an unfragmented IPv4 header carrying TCP inside @hdr_len.
 */
static size_t tso_parse_ipv4(const uint8_t *hdr, size_t hdr_len, size_t l3_off)
{
    if (hdr_len - l3_off < IP_HDR_LEN) {
        return 0;
    }

    const uint8_t *ip = hdr + l3_off;
    size_t ihl = (size_t)(ip[IP_VERSION_IHL_OFF] & 0xfU) * HDR_LEN_UNIT;

    if (ip[IP_VERSION_IHL_OFF] >> IP_VERSION_SHIFT != IP_VERSION ||
        ihl < IP_HDR_LEN || ihl > hdr_len - l3_off ||
        ip[IP_PROTOCOL_OFF] != IP_PROTOCOL_TCP ||
        (ernic_get_be16(ip + IP_FRAGMENT_OFF) & IP_FRAGMENT_MASK) != 0) {
        return 0;
    }
    return l3_off + ihl;
}

/* The same for the IPv6 header at @l3_off and its extension headers. */
static size_t tso_parse_ipv6(const uint8_t *hdr, size_t hdr_len, size_t l3_off)
{
    if (hdr_len - l3_off < IP6_HDR_LEN ||
        hdr[l3_off] >> IP6_VERSION_SHIFT != IP6_VERSION) {
        return 0;
    }

    uint8_t next = hdr[l3_off + IP6_NEXT_HDR_OFF];
    size_t off = l3_off + IP6_HDR_LEN;

    /* Each pass moves @off on by at least 8 bytes, so this ends. */
    while (next != IP_PROTOCOL_TCP) {
        if (next != IP6_NEXT_HDR_HOP_BY_HOP && next != IP6_NEXT_HDR_ROUTING &&
            next != IP6_NEXT_HDR_DST_OPTS) {
            return 0;
        }
        if (hdr_len - off < IP6_EXT_LEN_UNIT) {
            return 0;
        }

        const uint8_t *ext = hdr + off;
        size_t ext_len = ((size_t)ext[IP6_EXT_LEN_OFF] + 1U) * IP6_EXT_LEN_UNIT;

        if (ext_len > hdr_len - off || (next == IP6_NEXT_HDR_ROUTING &&
                                        ext[IP6_ROUTING_SEGS_LEFT_OFF] != 0)) {
            return 0;
        }
        next = ext[IP6_EXT_NEXT_HDR_OFF];
        off += ext_len;
    }
    return off;
}

bool ionic_eth_tso_parse(const uint8_t *hdr, size_t hdr_len,
                         struct ionic_eth_tso *t)
{
    if (hdr_len < ETH_HDR_LEN || hdr_len > UINT16_MAX) {
        return false;
    }

    size_t l3_off = ETH_HDR_LEN;
    uint16_t ethertype = ernic_get_be16(hdr + ETH_TYPE_OFF);

    if (is_vlan_ethertype(ethertype)) {
        if (hdr_len < ETH_HDR_LEN + ETH_VLAN_TAG_LEN) {
            return false;
        }
        /* A second tag is neither IP ethertype, and is refused below. */
        ethertype = ernic_get_be16(hdr + ETH_TYPE_OFF + ETH_VLAN_TAG_LEN);
        l3_off += ETH_VLAN_TAG_LEN;
    }

    size_t l4_off;
    bool ipv6 = ethertype == ETH_ETHERTYPE_IPV6;

    if (ipv6) {
        l4_off = tso_parse_ipv6(hdr, hdr_len, l3_off);
    } else if (ethertype == ETH_ETHERTYPE_IP) {
        l4_off = tso_parse_ipv4(hdr, hdr_len, l3_off);
    } else {
        return false;
    }
    if (l4_off == 0 || hdr_len - l4_off < TCP_HDR_LEN) {
        return false;
    }

    const uint8_t *tcp = hdr + l4_off;
    size_t tcp_hdr_len =
        (size_t)(tcp[TCP_DATA_OFFSET_OFF] >> TCP_DATA_OFFSET_SHIFT) *
        HDR_LEN_UNIT;

    if (tcp_hdr_len < TCP_HDR_LEN || tcp_hdr_len != hdr_len - l4_off ||
        (tcp[TCP_FLAGS_OFF] & (TCP_FLAG_SYN | TCP_FLAG_RST)) != 0) {
        return false;
    }

    /* Each fits: hdr_len is at most UINT16_MAX, and they are below it. */
    t->hdr_len = (uint16_t)hdr_len;
    t->l3_off = (uint16_t)l3_off;
    t->l4_off = (uint16_t)l4_off;
    t->ipv6 = ipv6;
    t->ip_id = ipv6 ? 0 : ernic_get_be16(hdr + l3_off + IP_ID_OFF);
    t->seq = ernic_get_be32(tcp + TCP_SEQ_OFF);
    t->tcp_flags = tcp[TCP_FLAGS_OFF];
    return true;
}

void ionic_eth_tso_segment(const struct ionic_eth_tso *t, uint8_t *frame,
                           size_t len, uint32_t index, uint32_t payload_off,
                           bool last)
{
    uint8_t *ip = frame + t->l3_off;
    uint8_t *tcp = frame + t->l4_off;
    size_t ip_len = len - t->l3_off;
    size_t tcp_len = len - t->l4_off;
    uint32_t sum;

    /* The lengths fit 16 bits because @len does; see ionic_eth_tx.h. */
    if (t->ipv6) {
        ernic_put_be16(ip + IP6_PAYLOAD_OFF, (uint16_t)(ip_len - IP6_HDR_LEN));
        sum = net_csum_add(0, ip + IP6_SRC_OFF, (size_t)2 * IP6_ADDR_LEN);
    } else {
        ernic_put_be16(ip + IP_TOTAL_LEN_OFF, (uint16_t)ip_len);
        /* The identification counts up from the template's, modulo 2^16. */
        ernic_put_be16(ip + IP_ID_OFF,
                       (uint16_t)((t->ip_id + index) & UINT16_MAX));
        ernic_put_be16(ip + IP_CHECKSUM_OFF, 0);
        ernic_put_be16(
            ip + IP_CHECKSUM_OFF,
            net_csum_fold(net_csum_add(0, ip, (size_t)t->l4_off - t->l3_off)));
        sum = net_csum_add(0, ip + IP_SRC_OFF, 2U * sizeof(uint32_t));
    }

    uint8_t flags = t->tcp_flags;

    if (!last) {
        flags &= (uint8_t) ~(TCP_FLAG_FIN | TCP_FLAG_PSH);
    }
    if (index != 0) {
        flags &= (uint8_t)~TCP_FLAG_CWR;
    }
    tcp[TCP_FLAGS_OFF] = flags;
    /* Sequence numbers wrap modulo 2^32, as this sum does. */
    ernic_put_be32(tcp + TCP_SEQ_OFF, t->seq + payload_off);

    /* The pseudo-header: both addresses, summed above, then the protocol
     * and the TCP length, each as a word of its own. */
    sum += IP_PROTOCOL_TCP;
    sum += (uint32_t)tcp_len;
    ernic_put_be16(tcp + TCP_CHECKSUM_OFF, 0);
    ernic_put_be16(tcp + TCP_CHECKSUM_OFF,
                   net_csum_fold(net_csum_add(sum, tcp, tcp_len)));
}
