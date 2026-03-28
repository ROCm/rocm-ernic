/*
 * TCP Connection State Management for rdma_cm
 * (loopback mode only)
 *
 * In-process TCP state machine used by the loopback CM
 * stub (pvrdma_eth_handle_cm_loopback) when there is no
 * real peer VM.  The TCP mesh backend does not use this
 * module; it forwards raw Ethernet frames to the peer
 * and lets the guest kernel TCP stack handle state.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TCP_CONN_H
#define TCP_CONN_H

#include <stdint.h>
#include <stdbool.h>
#include <glib.h>

/* Forward declaration */
struct tcp_header;

/* TCP connection states */
typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RECEIVED,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_CLOSING,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT,
} TcpState;

/* TCP connection structure */
typedef struct {
    uint32_t local_ip;    /* Local IP address */
    uint32_t remote_ip;   /* Remote IP address */
    uint16_t local_port;  /* Local port */
    uint16_t remote_port; /* Remote port */
    uint32_t local_seq;   /* Local sequence number */
    uint32_t remote_seq;  /* Remote sequence number */
    uint32_t local_ack;   /* Local acknowledgment number */
    uint32_t remote_ack;  /* Remote acknowledgment number */
    TcpState state;       /* Connection state */
    uint32_t iss;         /* Initial send sequence number */
    uint32_t irs;         /* Initial receive sequence number */
    bool is_server;       /* True if this is a server connection */
} TcpConnection;

/* Create a new TCP connection */
TcpConnection *tcp_conn_create(uint32_t local_ip, uint32_t remote_ip,
                               uint16_t local_port, uint16_t remote_port,
                               bool is_server);

/* Free TCP connection */
void tcp_conn_free(TcpConnection *conn);

/* Find TCP connection by 4-tuple */
TcpConnection *tcp_conn_find(GHashTable *conn_table, uint32_t local_ip,
                             uint32_t remote_ip, uint16_t local_port,
                             uint16_t remote_port);

/* Process TCP packet and update connection state */
int tcp_conn_process_packet(TcpConnection *conn,
                            const struct tcp_header *tcp_hdr, uint32_t seq,
                            uint32_t ack, uint8_t flags);

/* Generate TCP response packet */
int tcp_conn_generate_response(TcpConnection *conn, uint8_t flags,
                               const void *payload, size_t payload_len,
                               void *response_frame, size_t max_len,
                               uint32_t *response_len);

#endif /* TCP_CONN_H */
