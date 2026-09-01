/*
 * DHCP Server Module Implementation
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dhcp_server.h"
#include "from-qemu/hw/rdma/rdma_utils.h"
#include "net_headers.h" /* For htonl/ntohl */
#include "qemu/thread.h"
#include <string.h>
#include <time.h>
#include <errno.h>

/* MAC address comparison helper */
static gboolean mac_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, 6) == 0;
}

/* MAC address hash helper */
static guint mac_hash(gconstpointer key)
{
    const uint8_t *mac = (const uint8_t *)key;
    return mac[0] ^ mac[1] ^ mac[2] ^ mac[3] ^ mac[4] ^ mac[5];
}

DhcpServer *dhcp_server_create(uint32_t server_ip, uint32_t subnet_mask,
                               uint32_t router_ip, uint32_t dns_server,
                               uint32_t ip_pool_start, uint32_t ip_pool_end,
                               uint32_t lease_time)
{
    DhcpServer *server = g_new0(DhcpServer, 1);

    server->server_ip = server_ip;
    server->subnet_mask = subnet_mask;
    server->router_ip = router_ip;
    server->dns_server = dns_server;
    server->ip_pool_start = ip_pool_start;
    server->ip_pool_end = ip_pool_end;
    server->lease_time = lease_time;
    server->next_ip = ip_pool_start;

    server->allocations =
        g_hash_table_new_full(mac_hash, mac_equal, g_free, g_free);
    server->leases = g_hash_table_new_full(mac_hash, mac_equal, g_free, g_free);
    qemu_mutex_init(&server->lock);

    rdma_info_report("DHCP Server created: pool %08x-%08x, lease=%u", server_ip,
                     ip_pool_end, lease_time);

    return server;
}

void dhcp_server_destroy(DhcpServer *server)
{
    if (!server) {
        return;
    }

    qemu_mutex_destroy(&server->lock);
    g_hash_table_destroy(server->allocations);
    g_hash_table_destroy(server->leases);
    g_free(server);
}

/* DHCP magic cookie: 0x63825363 */
#define DHCP_MAGIC_COOKIE 0x63825363

/* Find DHCP option in packet */
static uint8_t *dhcp_find_option(const struct dhcp_packet *packet,
                                 uint8_t option_type)
{
    const uint8_t *options = packet->options;
    size_t i = 0;
    int opt_count = 0;

    /* Check for magic cookie at start of options field */
    if (options[0] == 0x63 && options[1] == 0x82 && options[2] == 0x53 &&
        options[3] == 0x63) {
        /* Skip magic cookie (4 bytes) */
        i = 4;
        rdma_info_report(
            "DHCP: Found magic cookie, starting options search at offset 4");
    }

    while (i < sizeof(packet->options)) {
        uint8_t opt = options[i];

        if (opt == DHCP_OPT_END) {
            rdma_info_report(
                "DHCP: Found END option at offset %zu (searched %d options)", i,
                opt_count);
            break;
        }
        if (opt == DHCP_OPT_PAD) {
            i++;
            continue;
        }

        if (i + 1 >= sizeof(packet->options)) {
            rdma_warn_report("DHCP: Options buffer overflow at offset %zu", i);
            break;
        }

        uint8_t opt_len = options[i + 1];
        opt_count++;

        if (opt == option_type) {
            rdma_info_report("DHCP: Found option %u at offset %zu (len=%u)",
                             option_type, i, opt_len);
            return (uint8_t *)&options[i];
        }

        i += 2 + opt_len;
    }

    rdma_warn_report("DHCP: Option %u not found (searched %d options, stopped "
                     "at offset %zu)",
                     option_type, opt_count, i);
    return NULL;
}

/* Add DHCP option to packet */
static void dhcp_add_option(uint8_t *options, size_t *offset, uint8_t type,
                            const void *data, uint8_t len)
{
    /* Check bounds: need space for type (1) + len (1) + data (len) */
    if (*offset + 2 + len > 312) {
        rdma_warn_report("DHCP: Options buffer overflow: offset=%zu len=%u",
                         *offset, len);
        return; /* Out of space */
    }

    size_t idx = *offset;
    options[idx++] = type;
    options[idx++] = len;
    memcpy(&options[idx], data, len);
    *offset = idx + len;
}

/* Allocate next available IP */
static uint32_t dhcp_allocate_ip(DhcpServer *server, const uint8_t *mac)
{
    uint32_t start_ip = server->ip_pool_start;
    uint32_t end_ip = server->ip_pool_end;
    uint32_t ip = server->next_ip;

    /* Try to find an available IP */
    for (uint32_t i = 0; i <= (end_ip - start_ip); i++) {
        /* Check if IP is already allocated */
        bool in_use = false;
        GHashTableIter iter;
        gpointer key, value;

        g_hash_table_iter_init(&iter, server->allocations);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            uint32_t *allocated_ip = (uint32_t *)value;
            if (*allocated_ip == ip) {
                in_use = true;
                break;
            }
        }

        if (!in_use) {
            /* Allocate this IP */
            uint8_t *mac_copy = g_malloc(6);
            uint32_t *ip_copy = g_new(uint32_t, 1);
            memcpy(mac_copy, mac, 6);
            *ip_copy = ip;

            g_hash_table_insert(server->allocations, mac_copy, ip_copy);

            /* Set lease expiry */
            time_t *expiry = g_new(time_t, 1);
            *expiry = time(NULL) + server->lease_time;
            uint8_t *mac_copy2 = g_malloc(6);
            memcpy(mac_copy2, mac, 6);
            g_hash_table_insert(server->leases, mac_copy2, expiry);

            server->next_ip = (ip == end_ip) ? start_ip : (ip + 1);

            rdma_info_report(
                "DHCP: Allocated IP %08x to MAC %02x:%02x:%02x:%02x:%02x:%02x",
                ip, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

            return ip;
        }

        ip = (ip == end_ip) ? start_ip : (ip + 1);
    }

    return 0; /* No IPs available */
}

size_t dhcp_server_process(DhcpServer *server,
                           const struct dhcp_packet *request,
                           size_t request_len, struct dhcp_packet *response,
                           size_t max_response_len)
{
    rdma_info_report("DHCP: dhcp_server_process called: request_len=%zu "
                     "max_response_len=%zu",
                     request_len, max_response_len);

    if (!server || !request || !response ||
        max_response_len < sizeof(*response)) {
        rdma_warn_report("DHCP: Invalid parameters: server=%p request=%p "
                         "response=%p max_len=%zu",
                         server, request, response, max_response_len);
        return 0;
    }

    /* The parser and dhcp_find_option() treat the request as a full,
     * fixed-size dhcp_packet (including the 312-byte options array), so a
     * short buffer would be read out of bounds. Reject anything smaller. */
    if (request_len < sizeof(*request)) {
        rdma_warn_report("DHCP: Request too short: %zu bytes (need %zu)",
                         request_len, sizeof(*request));
        return 0;
    }

    qemu_mutex_lock(&server->lock);

    /* Debug: Print first few bytes of options field */
    rdma_info_report(
        "DHCP: Options field start: %02x %02x %02x %02x %02x %02x %02x %02x",
        request->options[0], request->options[1], request->options[2],
        request->options[3], request->options[4], request->options[5],
        request->options[6], request->options[7]);

    /* Find message type option */
    uint8_t *msg_type_opt = dhcp_find_option(request, DHCP_OPT_MSG_TYPE);
    if (!msg_type_opt) {
        rdma_warn_report("DHCP: Message type option not found in request");
        qemu_mutex_unlock(&server->lock);
        return 0;
    }
    if (msg_type_opt[1] != 1) {
        rdma_warn_report(
            "DHCP: Invalid message type option length: %u (expected 1)",
            msg_type_opt[1]);
        qemu_mutex_unlock(&server->lock);
        return 0;
    }

    uint8_t msg_type = msg_type_opt[2];
    rdma_info_report("DHCP: Message type: %u", msg_type);

    /* Initialize response */
    memset(response, 0, sizeof(*response));
    response->op = 2;    /* BOOTREPLY */
    response->htype = 1; /* Ethernet */
    response->hlen = 6;
    response->xid = request->xid;
    memcpy(response->chaddr, request->chaddr, 16);
    response->siaddr = server->server_ip;

    uint8_t options[312] = {0};
    size_t opt_offset = 0;

    /* Add magic cookie to options */
    options[opt_offset++] = 0x63;
    options[opt_offset++] = 0x82;
    options[opt_offset++] = 0x53;
    options[opt_offset++] = 0x63;

    switch (msg_type) {
    case DHCP_MSG_DISCOVER: {
        /* Allocate IP and send OFFER */
        uint32_t yiaddr = dhcp_allocate_ip(server, request->chaddr);
        if (!yiaddr) {
            qemu_mutex_unlock(&server->lock);
            rdma_warn_report("DHCP: No IPs available for allocation");
            return 0; /* No IPs available */
        }

        response->yiaddr = yiaddr;
        rdma_info_report(
            "DHCP: Allocated IP %08x to MAC %02x:%02x:%02x:%02x:%02x:%02x",
            yiaddr, request->chaddr[0], request->chaddr[1], request->chaddr[2],
            request->chaddr[3], request->chaddr[4], request->chaddr[5]);

        /* Calculate broadcast address: (IP_address) | (~subnet_mask)
         * All values are in network byte order */
        uint32_t broadcast_addr = yiaddr | (~server->subnet_mask);

        /* Add options */
        uint8_t offer_type = DHCP_MSG_OFFER;
        dhcp_add_option(options, &opt_offset, DHCP_OPT_MSG_TYPE, &offer_type,
                        1);

        /* Subnet mask, server IP, router IP, DNS server, and broadcast address
         * are already in network byte order (from inet_addr() or calculation),
         * so use them directly */
        dhcp_add_option(options, &opt_offset, DHCP_OPT_SUBNET_MASK,
                        &server->subnet_mask, 4);
        rdma_info_report("DHCP: Added subnet mask option: %08x (255.255.255.0)",
                         server->subnet_mask);

        dhcp_add_option(options, &opt_offset, DHCP_OPT_BROADCAST_ADDRESS,
                        &broadcast_addr, 4);
        rdma_info_report("DHCP: Added broadcast address option: %08x",
                         broadcast_addr);

        dhcp_add_option(options, &opt_offset, DHCP_OPT_SERVER_ID,
                        &server->server_ip, 4);

        uint32_t lease_time = htonl(server->lease_time);
        dhcp_add_option(options, &opt_offset, DHCP_OPT_LEASE_TIME, &lease_time,
                        4);

        if (server->router_ip) {
            dhcp_add_option(options, &opt_offset, DHCP_OPT_ROUTER,
                            &server->router_ip, 4);
            rdma_info_report("DHCP: Added router option: %08x (192.168.100.1)",
                             server->router_ip);
        }

        if (server->dns_server) {
            dhcp_add_option(options, &opt_offset, DHCP_OPT_DNS_SERVER,
                            &server->dns_server, 4);
        }

        options[opt_offset++] = DHCP_OPT_END;
        memcpy(response->options, options, opt_offset);

        qemu_mutex_unlock(&server->lock);
        rdma_info_report("DHCP: Generated OFFER response: yiaddr=%08x "
                         "opt_len=%zu total_len=%zu",
                         response->yiaddr, opt_offset, sizeof(*response));
        return sizeof(*response);
    }

    case DHCP_MSG_REQUEST: {
        /* Check if requesting a specific IP */
        /* First check REQUESTED_IP option */
        uint8_t *req_ip_opt = dhcp_find_option(request, DHCP_OPT_REQUESTED_IP);
        uint32_t requested_ip = 0;

        if (req_ip_opt && req_ip_opt[1] == 4) {
            memcpy(&requested_ip, &req_ip_opt[2], 4);
            rdma_info_report("DHCP: REQUEST with REQUESTED_IP option: %08x",
                             requested_ip);
        }

        /* If no REQUESTED_IP option, check ciaddr field */
        if (requested_ip == 0 && request->ciaddr != 0) {
            requested_ip = request->ciaddr;
            rdma_info_report("DHCP: REQUEST using ciaddr: %08x", requested_ip);
        }

        /* Check if IP is allocated to this MAC */
        uint32_t *allocated_ip =
            g_hash_table_lookup(server->allocations, request->chaddr);

        if (allocated_ip) {
            rdma_info_report("DHCP: Found allocated IP %08x for MAC "
                             "%02x:%02x:%02x:%02x:%02x:%02x",
                             *allocated_ip, request->chaddr[0],
                             request->chaddr[1], request->chaddr[2],
                             request->chaddr[3], request->chaddr[4],
                             request->chaddr[5]);
        } else {
            rdma_warn_report("DHCP: No allocated IP found for MAC "
                             "%02x:%02x:%02x:%02x:%02x:%02x",
                             request->chaddr[0], request->chaddr[1],
                             request->chaddr[2], request->chaddr[3],
                             request->chaddr[4], request->chaddr[5]);
        }

        /* If no requested IP specified, use allocated IP if available */
        if (requested_ip == 0 && allocated_ip) {
            requested_ip = *allocated_ip;
            rdma_info_report("DHCP: Using allocated IP as requested IP: %08x",
                             requested_ip);
        }

        if (allocated_ip && *allocated_ip == requested_ip) {
            /* IP is already allocated to this MAC - send ACK */
            response->yiaddr = *allocated_ip;

            /* Calculate broadcast address: (IP_address) | (~subnet_mask)
             * All values are in network byte order */
            uint32_t broadcast_addr = *allocated_ip | (~server->subnet_mask);

            uint8_t ack_type = DHCP_MSG_ACK;
            dhcp_add_option(options, &opt_offset, DHCP_OPT_MSG_TYPE, &ack_type,
                            1);

            /* Subnet mask, server IP, router IP, DNS server, and broadcast
             * address are already in network byte order (from inet_addr() or
             * calculation), so use them directly */
            dhcp_add_option(options, &opt_offset, DHCP_OPT_SUBNET_MASK,
                            &server->subnet_mask, 4);
            rdma_info_report(
                "DHCP: Added subnet mask option: %08x (255.255.255.0)",
                server->subnet_mask);

            dhcp_add_option(options, &opt_offset, DHCP_OPT_BROADCAST_ADDRESS,
                            &broadcast_addr, 4);
            rdma_info_report("DHCP: Added broadcast address option: %08x",
                             broadcast_addr);

            dhcp_add_option(options, &opt_offset, DHCP_OPT_SERVER_ID,
                            &server->server_ip, 4);

            uint32_t lease_time = htonl(server->lease_time);
            dhcp_add_option(options, &opt_offset, DHCP_OPT_LEASE_TIME,
                            &lease_time, 4);

            if (server->router_ip) {
                dhcp_add_option(options, &opt_offset, DHCP_OPT_ROUTER,
                                &server->router_ip, 4);
                rdma_info_report(
                    "DHCP: Added router option: %08x (192.168.100.1)",
                    server->router_ip);
            }

            if (server->dns_server) {
                dhcp_add_option(options, &opt_offset, DHCP_OPT_DNS_SERVER,
                                &server->dns_server, 4);
            }

            options[opt_offset++] = DHCP_OPT_END;
            memcpy(response->options, options, opt_offset);

            qemu_mutex_unlock(&server->lock);
            rdma_info_report(
                "DHCP: Generated ACK response: yiaddr=%08x opt_len=%zu",
                response->yiaddr, opt_offset);
            return sizeof(*response);
        } else {
            /* IP not available or not allocated to this MAC - send NAK */
            /* Add magic cookie if not already added */
            if (opt_offset == 0) {
                options[opt_offset++] = 0x63;
                options[opt_offset++] = 0x82;
                options[opt_offset++] = 0x53;
                options[opt_offset++] = 0x63;
            }
            uint8_t nak_type = DHCP_MSG_NAK;
            dhcp_add_option(options, &opt_offset, DHCP_OPT_MSG_TYPE, &nak_type,
                            1);

            options[opt_offset++] = DHCP_OPT_END;
            memcpy(response->options, options, opt_offset);

            qemu_mutex_unlock(&server->lock);
            return sizeof(*response);
        }
    }

    case DHCP_MSG_RELEASE: {
        /* Release IP allocation */
        dhcp_server_release_ip(server, request->chaddr);
        qemu_mutex_unlock(&server->lock);
        return 0; /* No response needed for RELEASE */
    }

    default:
        qemu_mutex_unlock(&server->lock);
        return 0;
    }
}

uint32_t dhcp_server_get_allocated_ip(DhcpServer *server, const uint8_t *mac)
{
    if (!server) {
        return 0;
    }

    qemu_mutex_lock(&server->lock);
    uint32_t *ip = g_hash_table_lookup(server->allocations, mac);
    uint32_t result = ip ? *ip : 0;
    qemu_mutex_unlock(&server->lock);

    return result;
}

void dhcp_server_release_ip(DhcpServer *server, const uint8_t *mac)
{
    if (!server) {
        return;
    }

    qemu_mutex_lock(&server->lock);

    uint32_t *ip = g_hash_table_lookup(server->allocations, mac);
    if (ip) {
        rdma_info_report(
            "DHCP: Releasing IP %08x for MAC %02x:%02x:%02x:%02x:%02x:%02x",
            *ip, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        g_hash_table_remove(server->allocations, mac);
        g_hash_table_remove(server->leases, mac);
    }

    qemu_mutex_unlock(&server->lock);
}
