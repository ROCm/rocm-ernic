/*
 * TCP Connection State Management Implementation for rdma_cm
 * (loopback mode only -- see tcp_conn.h for details)
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "tcp_conn.h"
#include "net_headers.h"
#include "from-qemu/hw/rdma/rdma_utils.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>

/* Generate initial sequence number */
static uint32_t generate_iss(void)
{
    static uint32_t counter = 0;
    return (time(NULL) & 0xFFFFFFFF) + (++counter);
}

/* Create a new TCP connection */
TcpConnection *tcp_conn_create(uint32_t local_ip, uint32_t remote_ip,
                               uint16_t local_port, uint16_t remote_port,
                               bool is_server)
{
    TcpConnection *conn = g_new0(TcpConnection, 1);

    conn->local_ip = local_ip;
    conn->remote_ip = remote_ip;
    conn->local_port = local_port;
    conn->remote_port = remote_port;
    conn->is_server = is_server;
    conn->iss = generate_iss();
    conn->local_seq = conn->iss;
    conn->local_ack = 0;

    if (is_server) {
        conn->state = TCP_STATE_LISTEN;
    } else {
        conn->state = TCP_STATE_SYN_SENT;
    }

    return conn;
}

/* Free TCP connection */
void tcp_conn_free(TcpConnection *conn)
{
    if (conn) {
        g_free(conn);
    }
}

/* Generate hash key for connection lookup */
static uint64_t tcp_conn_hash_key(uint32_t local_ip, uint32_t remote_ip,
                                  uint16_t local_port, uint16_t remote_port)
{
    return ((uint64_t)local_ip << 32) | remote_ip |
           ((uint64_t)local_port << 48) | ((uint64_t)remote_port << 32);
}

/* Find TCP connection by 4-tuple */
TcpConnection *tcp_conn_find(GHashTable *conn_table, uint32_t local_ip,
                             uint32_t remote_ip, uint16_t local_port,
                             uint16_t remote_port)
{
    if (!conn_table) {
        return NULL;
    }

    uint64_t key =
        tcp_conn_hash_key(local_ip, remote_ip, local_port, remote_port);
    return g_hash_table_lookup(conn_table, GUINT_TO_POINTER(key));
}

/* Process TCP packet and update connection state */
int tcp_conn_process_packet(TcpConnection *conn,
                            const struct tcp_header *tcp_hdr, uint32_t seq,
                            uint32_t ack, uint8_t flags)
{
    if (!conn || !tcp_hdr) {
        return -1;
    }

    uint8_t syn_flag = (flags & TCP_FLAG_SYN) ? 1 : 0;
    uint8_t ack_flag = (flags & TCP_FLAG_ACK) ? 1 : 0;
    uint8_t fin_flag = (flags & TCP_FLAG_FIN) ? 1 : 0;
    uint8_t rst_flag = (flags & TCP_FLAG_RST) ? 1 : 0;

    /* Handle RST */
    if (rst_flag) {
        conn->state = TCP_STATE_CLOSED;
        return 0;
    }

    /* State machine */
    switch (conn->state) {
    case TCP_STATE_LISTEN:
        if (syn_flag && !ack_flag) {
            /* SYN received - move to SYN_RECEIVED */
            conn->irs = seq;
            conn->remote_seq = seq + 1;
            conn->local_ack = seq + 1;
            conn->state = TCP_STATE_SYN_RECEIVED;
            rdma_info_report("TCP: State transition: LISTEN -> SYN_RECEIVED");
            return 1; /* Need to send SYN-ACK */
        }
        break;

    case TCP_STATE_SYN_SENT:
        if (syn_flag && ack_flag) {
            /* SYN-ACK received */
            if (ack == conn->iss + 1) {
                conn->irs = seq;
                conn->remote_seq = seq + 1;
                conn->local_ack = seq + 1;
                conn->state = TCP_STATE_ESTABLISHED;
                return 1; /* Need to send ACK */
            }
        } else if (syn_flag && !ack_flag) {
            /* Simultaneous SYN - move to SYN_RECEIVED */
            conn->irs = seq;
            conn->remote_seq = seq + 1;
            conn->local_ack = seq + 1;
            conn->state = TCP_STATE_SYN_RECEIVED;
            return 1; /* Need to send SYN-ACK */
        }
        break;

    case TCP_STATE_SYN_RECEIVED:
        rdma_info_report("TCP: SYN_RECEIVED state: ack_flag=%u ack=%u "
                         "conn->iss=%u expected_ack=%u match=%s",
                         ack_flag, ack, conn->iss, conn->iss + 1,
                         (ack_flag && ack == conn->iss + 1) ? "YES" : "NO");
        if (ack_flag && ack == conn->iss + 1) {
            /* ACK received - connection established */
            conn->state = TCP_STATE_ESTABLISHED;
            rdma_info_report(
                "TCP: State transition: SYN_RECEIVED -> ESTABLISHED");
            return 0;
        }
        break;

    case TCP_STATE_ESTABLISHED:
        if (fin_flag) {
            /* FIN received */
            conn->remote_seq = seq + 1;
            conn->local_ack = seq + 1;
            conn->state = TCP_STATE_CLOSE_WAIT;
            return 1; /* Need to send ACK */
        } else if (ack_flag) {
            /* Data ACK - update local_ack to what remote is ACKing */
            conn->local_ack = ack;
            return 0;
        }
        break;

    case TCP_STATE_FIN_WAIT_1:
        if (ack_flag) {
            if (fin_flag) {
                conn->state = TCP_STATE_CLOSING;
            } else {
                conn->state = TCP_STATE_FIN_WAIT_2;
            }
            return 0;
        } else if (fin_flag && ack_flag) {
            conn->state = TCP_STATE_TIME_WAIT;
            return 1; /* Need to send ACK */
        }
        break;

    case TCP_STATE_FIN_WAIT_2:
        if (fin_flag) {
            conn->remote_seq = seq + 1;
            conn->local_ack = seq + 1;
            conn->state = TCP_STATE_TIME_WAIT;
            return 1; /* Need to send ACK */
        }
        break;

    case TCP_STATE_CLOSE_WAIT:
        /* Application should close - send FIN */
        break;

    case TCP_STATE_CLOSING:
        if (ack_flag) {
            conn->state = TCP_STATE_TIME_WAIT;
            return 0;
        }
        break;

    case TCP_STATE_LAST_ACK:
        if (ack_flag) {
            conn->state = TCP_STATE_CLOSED;
            return 0;
        }
        break;

    case TCP_STATE_TIME_WAIT:
        /* Wait for 2MSL - simplified: move to CLOSED */
        conn->state = TCP_STATE_CLOSED;
        return 0;

    case TCP_STATE_CLOSED:
        break;
    }

    return 0;
}

/* Generate TCP response packet */
int tcp_conn_generate_response(TcpConnection *conn, uint8_t flags,
                               const void *payload, size_t payload_len,
                               void *response_frame, size_t max_len,
                               uint32_t *response_len)
{
    if (!conn || !response_frame || !response_len) {
        return -1;
    }

    /* Calculate frame size */
    size_t eth_hdr_len = sizeof(struct eth_header);
    size_t ip_hdr_len = 20;
    size_t tcp_hdr_len = 20;
    size_t total_len = eth_hdr_len + ip_hdr_len + tcp_hdr_len + payload_len;

    if (total_len > max_len) {
        return -1;
    }

    uint8_t *frame = (uint8_t *)response_frame;

    /* Ethernet header */
    struct eth_header *eth_hdr = (struct eth_header *)frame;
    /* Will be filled by caller with MAC addresses */
    eth_hdr->ethertype = htons(ETH_ETHERTYPE_IP);

    /* IP header */
    struct ip_header *ip_hdr = (struct ip_header *)(frame + eth_hdr_len);
    ip_hdr->version_ihl = 0x45;
    ip_hdr->tos = 0;
    ip_hdr->total_len = htons(ip_hdr_len + tcp_hdr_len + payload_len);
    ip_hdr->id = htons(0);
    ip_hdr->frag_off = 0;
    ip_hdr->ttl = 64;
    ip_hdr->protocol = IP_PROTOCOL_TCP;
    ip_hdr->checksum = 0;
    ip_hdr->src_ip = conn->local_ip;
    ip_hdr->dst_ip = conn->remote_ip;
    ip_hdr->checksum = ip_checksum(ip_hdr, ip_hdr_len);

    /* TCP header */
    struct tcp_header *tcp_hdr =
        (struct tcp_header *)(frame + eth_hdr_len + ip_hdr_len);
    tcp_hdr->src_port = htons(conn->local_port);
    tcp_hdr->dst_port = htons(conn->remote_port);
    tcp_hdr->seq = htonl(conn->local_seq);
    tcp_hdr->ack = htonl(conn->local_ack);
    tcp_hdr->data_off = (tcp_hdr_len / 4) << 4;
    tcp_hdr->flags = flags;
    tcp_hdr->window = htons(65535);
    tcp_hdr->checksum = 0;
    tcp_hdr->urg_ptr = 0;

    /* Copy payload if present */
    if (payload && payload_len > 0) {
        memcpy(frame + eth_hdr_len + ip_hdr_len + tcp_hdr_len, payload,
               payload_len);
        conn->local_seq += payload_len;
    }

    /* Calculate TCP checksum */
    tcp_hdr->checksum = tcp_checksum(ip_hdr, tcp_hdr, payload, payload_len);

    /* Update sequence number for SYN/FIN */
    if (flags & TCP_FLAG_SYN) {
        conn->local_seq++;
    }
    if (flags & TCP_FLAG_FIN) {
        conn->local_seq++;
    }

    *response_len = total_len;
    return 0;
}
