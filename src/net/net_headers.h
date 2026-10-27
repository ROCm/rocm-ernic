/*
 * Network Header Layouts
 *
 * Ethernet, ARP, IPv4, ICMP and TCP headers, for the S3 target's TCP/IP
 * endpoint; the size of the RoCEv2 global route header; and the Internet
 * checksum, which the ionic datapath also uses to build a GRH.  Every
 * multi-byte field is big-endian.
 *
 * Offsets are spelled out as macros rather than packed structs: a frame is
 * a byte buffer, a header in it can start at any alignment, and a struct
 * laid over it would need a compiler extension to match the wire, so the
 * accessors in byteorder.h are the only sanctioned way to read and write
 * a field.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NET_HEADERS_H
#define NET_HEADERS_H

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Ethernet header
 *
 *   [0:5]   destination MAC
 *   [6:11]  source MAC
 *   [12:13] be16 ethertype
 * -------------------------------------------------------------------------
 */

#define ETH_HDR_LEN  14
#define ETH_DST_OFF  0
#define ETH_SRC_OFF  6
#define ETH_TYPE_OFF 12

#define ETH_ETHERTYPE_IP  0x0800
#define ETH_ETHERTYPE_ARP 0x0806

/* -------------------------------------------------------------------------
 * ARP, for Ethernet and IPv4
 *
 *   [0:1]   be16 hardware type (1 = Ethernet)
 *   [2:3]   be16 protocol type (0x0800 = IPv4)
 *   [4]     hardware address length (6)
 *   [5]     protocol address length (4)
 *   [6:7]   be16 operation
 *   [8:13]  sender MAC
 *   [14:17] be32 sender IPv4 address
 *   [18:23] target MAC
 *   [24:27] be32 target IPv4 address
 * -------------------------------------------------------------------------
 */

#define ARP_LEN            28
#define ARP_HW_TYPE_OFF    0
#define ARP_PROTO_TYPE_OFF 2
#define ARP_HW_LEN_OFF     4
#define ARP_PROTO_LEN_OFF  5
#define ARP_OP_OFF         6
#define ARP_SENDER_MAC_OFF 8
#define ARP_SENDER_IP_OFF  14
#define ARP_TARGET_MAC_OFF 18
#define ARP_TARGET_IP_OFF  24

#define ARP_HW_TYPE_ETHERNET 1
#define ARP_OP_REQUEST       1
#define ARP_OP_REPLY         2

/* -------------------------------------------------------------------------
 * IPv4 header; options, if any, follow it
 *
 *   [0]     version (high nibble) and header length in words (low nibble)
 *   [1]     type of service
 *   [2:3]   be16 total length
 *   [4:5]   be16 identification
 *   [6:7]   be16 flags (high 3 bits) and fragment offset
 *   [8]     time to live
 *   [9]     protocol
 *   [10:11] be16 header checksum
 *   [12:15] be32 source address
 *   [16:19] be32 destination address
 * -------------------------------------------------------------------------
 */

#define IP_HDR_LEN         20
#define IP_VERSION_IHL_OFF 0
#define IP_TOS_OFF         1
#define IP_TOTAL_LEN_OFF   2
#define IP_ID_OFF          4
#define IP_FRAGMENT_OFF    6
#define IP_TTL_OFF         8
#define IP_PROTOCOL_OFF    9
#define IP_CHECKSUM_OFF    10
#define IP_SRC_OFF         12
#define IP_DST_OFF         16

#define IP_PROTOCOL_TCP  6
#define IP_PROTOCOL_UDP  17
#define IP_PROTOCOL_ICMP 1

/* -------------------------------------------------------------------------
 * ICMP echo header; the echoed data follows it
 *
 *   [0]     type
 *   [1]     code
 *   [2:3]   be16 checksum
 *   [4:5]   be16 identifier
 *   [6:7]   be16 sequence number
 * -------------------------------------------------------------------------
 */

#define ICMP_HDR_LEN      8
#define ICMP_TYPE_OFF     0
#define ICMP_CODE_OFF     1
#define ICMP_CHECKSUM_OFF 2
#define ICMP_ID_OFF       4
#define ICMP_SEQ_OFF      6

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

/* -------------------------------------------------------------------------
 * TCP header; options, if any, follow it
 *
 *   [0:1]   be16 source port
 *   [2:3]   be16 destination port
 *   [4:7]   be32 sequence number
 *   [8:11]  be32 acknowledgment number
 *   [12]    header length in words (high nibble)
 *   [13]    flags
 *   [14:15] be16 window
 *   [16:17] be16 checksum
 *   [18:19] be16 urgent pointer
 * -------------------------------------------------------------------------
 */

#define TCP_HDR_LEN         20
#define TCP_SRC_PORT_OFF    0
#define TCP_DST_PORT_OFF    2
#define TCP_SEQ_OFF         4
#define TCP_ACK_OFF         8
#define TCP_DATA_OFFSET_OFF 12
#define TCP_FLAGS_OFF       13
#define TCP_WINDOW_OFF      14
#define TCP_CHECKSUM_OFF    16
#define TCP_URG_PTR_OFF     18

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

/* -------------------------------------------------------------------------
 * RoCEv2 global route header
 *
 * A received UD datagram is preceded by a 40-byte global route header: an
 * IPv6 header, or for RoCEv2 over IPv4, 20 bytes of zeros and then the
 * IPv4 header.
 * -------------------------------------------------------------------------
 */

#define IB_GRH_SIZE 40

/* -------------------------------------------------------------------------
 * The Internet checksum (RFC 1071)
 *
 * In two steps, so that a pseudo-header can be summed apart from the data
 * it covers: net_csum_add() adds @len bytes to @sum as big-endian 16-bit
 * words, padding an odd last byte with zero, and net_csum_fold() turns the
 * total into the checksum, which goes on the wire with ernic_put_be16().
 * The bytes are read one at a time, so @data needn't be aligned.
 *
 * A computed zero is returned as-is.  Substituting 0xffff for it is the
 * UDP rule; in TCP, ICMP and IPv4 a zero checksum is a legal value, and
 * substituting for it makes roughly one segment in 65536 arrive corrupt.
 * -------------------------------------------------------------------------
 */

/*
 * Returns the sum folded to 16 bits.  It adds at most 0xffff for each two
 * bytes before folding, so the sum can't wrap for any @len an IPv4
 * datagram can have, given a @sum that fits in 30 bits.
 */
static inline uint32_t net_csum_add(uint32_t sum, const uint8_t *data,
                                    size_t len)
{
    while (len > 1) {
        sum += ((uint32_t)data[0] << 8) | data[1];
        data += 2;
        len -= 2;
    }
    if (len > 0) {
        sum += (uint32_t)data[0] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return sum;
}

static inline uint16_t net_csum_fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

#endif /* NET_HEADERS_H */
