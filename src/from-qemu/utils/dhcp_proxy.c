/*
 * DHCP Proxy/Client Module Implementation
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "dhcp_proxy.h"
#include "from-qemu/hw/rdma/rdma_utils.h"
#include "net_headers.h" /* For htonl/ntohl */
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

/* TCP message types (must match rdma_backend_tcp.c) */
#define TCP_MSG_DHCP_REQUEST  11
#define TCP_MSG_DHCP_RESPONSE 12

/* TCP message header (must match rdma_backend_tcp.c) */
typedef struct {
    uint32_t magic;       /* Protocol magic (0x52444D41 = "RDMA") */
    uint32_t msg_type;    /* Message type */
    uint32_t msg_len;     /* Payload length */
    uint32_t seq;         /* Sequence number */
    uint32_t src_node_id; /* Source node ID */
    uint32_t dst_node_id; /* Destination node ID */
    uint32_t src_qpn;     /* Source QP number */
    uint32_t dst_qpn;     /* Destination QP number */
} __attribute__((packed)) TcpMsgHeader;

DhcpProxy *dhcp_proxy_create(int manager_sockfd, uint32_t server_ip)
{
    DhcpProxy *proxy = g_new0(DhcpProxy, 1);
    proxy->manager_sockfd = manager_sockfd;
    proxy->server_ip = server_ip;

    rdma_info_report("DHCP Proxy created (manager_sockfd=%d)", manager_sockfd);

    return proxy;
}

void dhcp_proxy_destroy(DhcpProxy *proxy)
{
    if (proxy) {
        g_free(proxy);
    }
}

size_t dhcp_proxy_forward_request(DhcpProxy *proxy,
                                  const struct dhcp_packet *request,
                                  size_t request_len,
                                  struct dhcp_packet *response,
                                  size_t max_response_len)
{
    if (!proxy || !request || !response ||
        max_response_len < sizeof(*response)) {
        return 0;
    }

    if (proxy->manager_sockfd < 0) {
        rdma_error_report("DHCP Proxy: Invalid manager socket");
        return 0;
    }

    /* Send DHCP request to manager via TCP */
    /* Use TCP_MSG_DHCP_REQUEST message type */
    TcpMsgHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = htonl(0x52444D41); /* "RDMA" */
    hdr.msg_type = TCP_MSG_DHCP_REQUEST;
    hdr.msg_len = htonl(request_len);
    hdr.seq = htonl(0); /* Sequence not critical for DHCP */
    hdr.src_node_id = 0;
    hdr.dst_node_id = 0;
    hdr.src_qpn = 0;
    hdr.dst_qpn = 0;

    /* Send header */
    ssize_t sent = send(proxy->manager_sockfd, &hdr, sizeof(hdr), 0);
    if (sent != sizeof(hdr)) {
        rdma_error_report("DHCP Proxy: Failed to send header: %s",
                          strerror(errno));
        return 0;
    }

    /* Send DHCP packet */
    sent = send(proxy->manager_sockfd, request, request_len, 0);
    if (sent != (ssize_t)request_len) {
        rdma_error_report("DHCP Proxy: Failed to send DHCP packet: %s",
                          strerror(errno));
        return 0;
    }

    /* Receive response header */
    TcpMsgHeader resp_hdr;
    ssize_t received =
        recv(proxy->manager_sockfd, &resp_hdr, sizeof(resp_hdr), MSG_WAITALL);
    if (received != sizeof(resp_hdr)) {
        rdma_error_report("DHCP Proxy: Failed to receive response header: %s",
                          strerror(errno));
        return 0;
    }

    if (resp_hdr.magic != htonl(0x52444D41) ||
        resp_hdr.msg_type != TCP_MSG_DHCP_RESPONSE) {
        rdma_error_report("DHCP Proxy: Invalid response header");
        return 0;
    }

    uint32_t resp_len = ntohl(resp_hdr.msg_len);
    if (resp_len > max_response_len) {
        rdma_error_report("DHCP Proxy: Response too large (%u > %zu)", resp_len,
                          max_response_len);
        return 0;
    }

    /* Receive DHCP response */
    received = recv(proxy->manager_sockfd, response, resp_len, MSG_WAITALL);
    if (received != (ssize_t)resp_len) {
        rdma_error_report("DHCP Proxy: Failed to receive DHCP response: %s",
                          strerror(errno));
        return 0;
    }

    rdma_info_report("DHCP Proxy: Received response from manager (%u bytes)",
                     resp_len);

    return resp_len;
}
