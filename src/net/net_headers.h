/*
 * Network Header Layouts
 *
 * Ethernet, ARP, IPv4, ICMP and TCP headers, for the S3 target's TCP/IP
 * endpoint, and the IPv4 header checksum, which the ionic datapath also
 * uses to build a RoCEv2 GRH.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NET_HEADERS_H
#define NET_HEADERS_H

#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>

/* Ethernet header (14 bytes) */
struct eth_header {
    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint16_t ethertype; /* Network byte order */
} __attribute__((packed));

#define ETH_ETHERTYPE_IP  0x0800
#define ETH_ETHERTYPE_ARP 0x0806

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
    uint16_t checksum = ~sum & 0xFFFFu;
    return htons(checksum);
}

#endif /* NET_HEADERS_H */
