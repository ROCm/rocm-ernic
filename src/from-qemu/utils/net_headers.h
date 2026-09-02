/*
 * Network Header Parsing Utilities
 *
 * Lightweight TCP/IP header parsing for Ethernet frame processing.
 * Used by DHCP server and rdma_cm protocol handlers.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NET_HEADERS_H
#define NET_HEADERS_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Ethernet header (14 bytes) */
struct eth_header {
    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint16_t ethertype; /* Network byte order */
} __attribute__((packed));

#define ETH_ETHERTYPE_IP   0x0800
#define ETH_ETHERTYPE_ARP  0x0806
#define ETH_ETHERTYPE_IPV6 0x86DD

/* ARP header (28 bytes for Ethernet/IPv4) */
struct arp_header {
    uint16_t hw_type; /* Hardware type (1 = Ethernet) - Network byte order */
    uint16_t
        proto_type; /* Protocol type (0x0800 = IPv4) - Network byte order */
    uint8_t hw_addr_len;    /* Hardware address length (6 for Ethernet) */
    uint8_t proto_addr_len; /* Protocol address length (4 for IPv4) */
    uint16_t op; /* Operation (1 = request, 2 = reply) - Network byte order */
    uint8_t sender_hw_addr[6]; /* Sender hardware address */
    uint32_t
        sender_proto_addr; /* Sender protocol address - Network byte order */
    uint8_t target_hw_addr[6]; /* Target hardware address */
    uint32_t
        target_proto_addr; /* Target protocol address - Network byte order */
} __attribute__((packed));

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

/* IP header (20 bytes minimum, can be longer with options) */
struct ip_header {
    uint8_t version_ihl; /* Version (4 bits) + IHL (4 bits) */
    uint8_t tos;
    uint16_t total_len; /* Network byte order */
    uint16_t id;        /* Network byte order */
    uint16_t frag_off;  /* Network byte order */
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum; /* Network byte order */
    uint32_t src_ip;   /* Network byte order */
    uint32_t dst_ip;   /* Network byte order */
} __attribute__((packed));

#define IP_PROTOCOL_TCP  6
#define IP_PROTOCOL_UDP  17
#define IP_PROTOCOL_ICMP 1

/* ICMP header (8 bytes minimum) */
struct icmp_header {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;   /* Network byte order */
    uint16_t identifier; /* Network byte order */
    uint16_t sequence;   /* Network byte order */
    /* Data follows */
} __attribute__((packed));

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

/* TCP header (20 bytes minimum, can be longer with options) */
struct tcp_header {
    uint16_t src_port; /* Network byte order */
    uint16_t dst_port; /* Network byte order */
    uint32_t seq;      /* Network byte order */
    uint32_t ack;      /* Network byte order */
    uint8_t data_off;  /* Data offset (4 bits) + reserved (4 bits) */
    uint8_t flags;
    uint16_t window;   /* Network byte order */
    uint16_t checksum; /* Network byte order */
    uint16_t urg_ptr;  /* Network byte order */
} __attribute__((packed));

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

#define TCP_PORT_RDMA_CM 18515

/* UDP header (8 bytes) */
struct udp_header {
    uint16_t src_port; /* Network byte order */
    uint16_t dst_port; /* Network byte order */
    uint16_t len;      /* Network byte order */
    uint16_t checksum; /* Network byte order */
} __attribute__((packed));

#define UDP_PORT_DHCP_SERVER 67
#define UDP_PORT_DHCP_CLIENT 68

/* Helper functions */
static inline uint16_t ntohs(uint16_t val)
{
    return ((val & 0xFF00) >> 8) | ((val & 0x00FF) << 8);
}

static inline uint16_t htons(uint16_t val)
{
    return ntohs(val);
}

static inline uint32_t ntohl(uint32_t val)
{
    return ((val & 0xFF000000) >> 24) | ((val & 0x00FF0000) >> 8) |
           ((val & 0x0000FF00) << 8) | ((val & 0x000000FF) << 24);
}

static inline uint32_t htonl(uint32_t val)
{
    return ntohl(val);
}

/* Parse Ethernet header from frame */
static inline bool parse_eth_header(const void *frame, size_t len,
                                    struct eth_header **eth_hdr)
{
    if (len < sizeof(struct eth_header)) {
        return false;
    }
    *eth_hdr = (struct eth_header *)frame;
    return true;
}

/* Parse IP header from frame (after Ethernet header) */
static inline bool parse_ip_header(const void *frame, size_t len,
                                   struct eth_header *eth_hdr,
                                   struct ip_header **ip_hdr)
{
    if (ntohs(eth_hdr->ethertype) != ETH_ETHERTYPE_IP) {
        return false;
    }
    size_t ip_offset = sizeof(struct eth_header);
    if (len < ip_offset + sizeof(struct ip_header)) {
        return false;
    }
    *ip_hdr = (struct ip_header *)((uint8_t *)frame + ip_offset);
    return true;
}

/* Parse TCP header from frame (after IP header) */
static inline bool parse_tcp_header(const void *frame, size_t len,
                                    struct ip_header *ip_hdr,
                                    struct tcp_header **tcp_hdr,
                                    size_t *tcp_offset)
{
    if (ip_hdr->protocol != IP_PROTOCOL_TCP) {
        return false;
    }
    size_t ip_hdr_len = (ip_hdr->version_ihl & 0x0F) * 4;
    *tcp_offset = sizeof(struct eth_header) + ip_hdr_len;
    if (len < *tcp_offset + sizeof(struct tcp_header)) {
        return false;
    }
    *tcp_hdr = (struct tcp_header *)((uint8_t *)frame + *tcp_offset);
    return true;
}

/* Parse UDP header from frame (after IP header) */
static inline bool parse_udp_header(const void *frame, size_t len,
                                    struct ip_header *ip_hdr,
                                    struct udp_header **udp_hdr,
                                    size_t *udp_offset)
{
    if (ip_hdr->protocol != IP_PROTOCOL_UDP) {
        return false;
    }
    size_t ip_hdr_len = (ip_hdr->version_ihl & 0x0F) * 4;
    *udp_offset = sizeof(struct eth_header) + ip_hdr_len;
    if (len < *udp_offset + sizeof(struct udp_header)) {
        return false;
    }
    *udp_hdr = (struct udp_header *)((uint8_t *)frame + *udp_offset);
    return true;
}

/* Get TCP payload offset */
static inline size_t get_tcp_payload_offset(struct tcp_header *tcp_hdr,
                                            size_t tcp_offset)
{
    uint8_t data_off = (tcp_hdr->data_off >> 4) & 0x0F;
    return tcp_offset + (data_off * 4);
}

/* Get UDP payload offset */
static inline size_t get_udp_payload_offset(size_t udp_offset)
{
    return udp_offset + sizeof(struct udp_header);
}

/* Calculate IP header checksum */
static inline uint16_t ip_checksum(const void *data, size_t len)
{
    const uint16_t *words = (const uint16_t *)data;
    uint32_t sum = 0;
    size_t i;

    /* Sum all 16-bit words */
    for (i = 0; i < len / 2; i++) {
        sum += ntohs(words[i]);
    }

    /* Add carry bits */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    /* One's complement */
    return htons(~(uint16_t)sum);
}

/* Calculate UDP checksum (includes pseudo-header) */
static inline uint16_t udp_checksum(const struct ip_header *ip_hdr,
                                    const struct udp_header *udp_hdr,
                                    const void *payload, size_t payload_len)
{
    uint32_t sum = 0;
    uint16_t udp_len = ntohs(udp_hdr->len);

    /* A NULL payload means "no payload"; treat it as zero-length so the
     * pseudo-header length and the summation stay consistent. */
    if (payload == NULL) {
        payload_len = 0;
    }

    /* Pseudo-header: src IP, dst IP, protocol, UDP length */
    sum += (ntohl(ip_hdr->src_ip) >> 16) & 0xFFFF;
    sum += ntohl(ip_hdr->src_ip) & 0xFFFF;
    sum += (ntohl(ip_hdr->dst_ip) >> 16) & 0xFFFF;
    sum += ntohl(ip_hdr->dst_ip) & 0xFFFF;
    sum += ip_hdr->protocol;
    sum += udp_len;

    /* UDP header (with checksum field set to 0) */
    sum += ntohs(udp_hdr->src_port);
    sum += ntohs(udp_hdr->dst_port);
    sum += udp_len;
    /* Skip checksum field (set to 0) */

    /* UDP payload */
    const uint16_t *payload_words = (const uint16_t *)payload;
    size_t i;
    for (i = 0; i < payload_len / 2; i++) {
        sum += ntohs(payload_words[i]);
    }

    /* Add odd byte if present */
    if (payload_len % 2) {
        sum += ((const uint8_t *)payload)[payload_len - 1] << 8;
    }

    /* Add carry bits */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    /* One's complement */
    uint16_t checksum = ~(uint16_t)sum;
    return htons(checksum == 0 ? 0xFFFF : checksum);
}

/* Calculate TCP checksum (includes pseudo-header) */
static inline uint16_t tcp_checksum(const struct ip_header *ip_hdr,
                                    const struct tcp_header *tcp_hdr,
                                    const void *payload, size_t payload_len)
{
    uint32_t sum = 0;

    /* A NULL payload means "no payload"; treat it as zero-length so the
     * pseudo-header length and the summation stay consistent. */
    if (payload == NULL) {
        payload_len = 0;
    }

    uint16_t tcp_len = (tcp_hdr->data_off >> 4) * 4 + payload_len;

    /* Pseudo-header: src IP, dst IP, protocol, TCP length */
    sum += (ntohl(ip_hdr->src_ip) >> 16) & 0xFFFF;
    sum += ntohl(ip_hdr->src_ip) & 0xFFFF;
    sum += (ntohl(ip_hdr->dst_ip) >> 16) & 0xFFFF;
    sum += ntohl(ip_hdr->dst_ip) & 0xFFFF;
    sum += ip_hdr->protocol;
    sum += tcp_len;

    /* TCP header (with checksum field set to 0) */
    sum += ntohs(tcp_hdr->src_port);
    sum += ntohs(tcp_hdr->dst_port);
    sum += (ntohl(tcp_hdr->seq) >> 16) & 0xFFFF;
    sum += ntohl(tcp_hdr->seq) & 0xFFFF;
    sum += (ntohl(tcp_hdr->ack) >> 16) & 0xFFFF;
    sum += ntohl(tcp_hdr->ack) & 0xFFFF;
    sum += ((tcp_hdr->data_off & 0xF0) << 8) | tcp_hdr->flags;
    sum += ntohs(tcp_hdr->window);
    /* Skip checksum field (set to 0) */
    sum += ntohs(tcp_hdr->urg_ptr);

    /* TCP payload */
    const uint16_t *payload_words = (const uint16_t *)payload;
    size_t i;
    for (i = 0; i < payload_len / 2; i++) {
        sum += ntohs(payload_words[i]);
    }

    /* Add odd byte if present */
    if (payload_len % 2) {
        sum += ((const uint8_t *)payload)[payload_len - 1] << 8;
    }

    /* Add carry bits */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    /* One's complement */
    uint16_t checksum = ~(uint16_t)sum;
    return htons(checksum == 0 ? 0xFFFF : checksum);
}

#endif /* NET_HEADERS_H */
