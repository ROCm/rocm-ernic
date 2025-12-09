/*
 * DHCP Proxy/Client Module
 *
 * Forwards DHCP requests from VM's netdev to manager via TCP socket.
 * Used by TCP backend worker nodes.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef DHCP_PROXY_H
#define DHCP_PROXY_H

#include <stdint.h>
#include <stdbool.h>
#include "dhcp_server.h" /* For dhcp_packet structure */

/* DHCP proxy context */
typedef struct {
    int manager_sockfd; /* TCP socket to manager */
    uint32_t server_ip; /* Manager's IP (for DHCP responses) */
} DhcpProxy;

/* Create DHCP proxy */
DhcpProxy *dhcp_proxy_create(int manager_sockfd, uint32_t server_ip);

/* Destroy DHCP proxy */
void dhcp_proxy_destroy(DhcpProxy *proxy);

/* Forward DHCP request to manager and get response
 * Returns: response packet size, or 0 on error
 */
size_t dhcp_proxy_forward_request(DhcpProxy *proxy,
                                  const struct dhcp_packet *request,
                                  size_t request_len,
                                  struct dhcp_packet *response,
                                  size_t max_response_len);

#endif /* DHCP_PROXY_H */
