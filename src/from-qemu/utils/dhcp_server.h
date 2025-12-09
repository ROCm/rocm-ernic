/*
 * DHCP Server Module
 *
 * Minimal DHCP server implementation for IP address allocation.
 * Used by loopback mode and TCP backend manager.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef DHCP_SERVER_H
#define DHCP_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <glib.h>
#include "qemu/thread.h"

/* DHCP message types */
#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_DECLINE  4
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6
#define DHCP_MSG_RELEASE  7
#define DHCP_MSG_INFORM   8

/* DHCP options */
#define DHCP_OPT_PAD               0
#define DHCP_OPT_SUBNET_MASK       1
#define DHCP_OPT_ROUTER            3
#define DHCP_OPT_DNS_SERVER        6
#define DHCP_OPT_HOSTNAME          12
#define DHCP_OPT_BROADCAST_ADDRESS 28
#define DHCP_OPT_REQUESTED_IP      50
#define DHCP_OPT_LEASE_TIME        51
#define DHCP_OPT_MSG_TYPE          53
#define DHCP_OPT_SERVER_ID         54
#define DHCP_OPT_END               255

/* DHCP packet structure (simplified) */
struct dhcp_packet {
    uint8_t op;           /* Message op code (1=BOOTREQUEST, 2=BOOTREPLY) */
    uint8_t htype;        /* Hardware address type (1=Ethernet) */
    uint8_t hlen;         /* Hardware address length (6 for Ethernet) */
    uint8_t hops;         /* Hops (0) */
    uint32_t xid;         /* Transaction ID (network byte order) */
    uint16_t secs;        /* Seconds elapsed (network byte order) */
    uint16_t flags;       /* Flags (network byte order) */
    uint32_t ciaddr;      /* Client IP address (network byte order) */
    uint32_t yiaddr;      /* Your IP address (network byte order) */
    uint32_t siaddr;      /* Server IP address (network byte order) */
    uint32_t giaddr;      /* Gateway IP address (network byte order) */
    uint8_t chaddr[16];   /* Client hardware address */
    uint8_t sname[64];    /* Server name */
    uint8_t file[128];    /* Boot file name */
    uint8_t options[312]; /* Options field */
} __attribute__((packed));

/* DHCP server context */
typedef struct {
    uint32_t server_ip;     /* Server IP address (network byte order) */
    uint32_t subnet_mask;   /* Subnet mask (network byte order) */
    uint32_t router_ip;     /* Router IP (network byte order) */
    uint32_t dns_server;    /* DNS server IP (network byte order) */
    uint32_t ip_pool_start; /* IP pool start (network byte order) */
    uint32_t ip_pool_end;   /* IP pool end (network byte order) */
    uint32_t lease_time;    /* Lease time in seconds */

    /* IP allocation tracking */
    GHashTable *allocations; /* MAC -> allocated IP */
    GHashTable *leases;      /* MAC -> lease expiry time */
    QemuMutex lock;

    uint32_t next_ip; /* Next IP to allocate */
} DhcpServer;

/* Create DHCP server */
DhcpServer *dhcp_server_create(uint32_t server_ip, uint32_t subnet_mask,
                               uint32_t router_ip, uint32_t dns_server,
                               uint32_t ip_pool_start, uint32_t ip_pool_end,
                               uint32_t lease_time);

/* Destroy DHCP server */
void dhcp_server_destroy(DhcpServer *server);

/* Process DHCP request and generate response
 * Returns: response packet size, or 0 on error
 */
size_t dhcp_server_process(DhcpServer *server,
                           const struct dhcp_packet *request,
                           size_t request_len, struct dhcp_packet *response,
                           size_t max_response_len);

/* Get allocated IP for a MAC address (or 0 if not allocated) */
uint32_t dhcp_server_get_allocated_ip(DhcpServer *server, const uint8_t *mac);

/* Release IP allocation for a MAC address */
void dhcp_server_release_ip(DhcpServer *server, const uint8_t *mac);

#endif /* DHCP_SERVER_H */
