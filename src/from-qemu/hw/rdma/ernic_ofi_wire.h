/*
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * libfabric (OFI) MSG/tcp helpers for the rocm-ernic mesh wire protocol.
 */

#ifndef ERNIC_OFI_WIRE_H
#define ERNIC_OFI_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "rdma_tcp_proto.h"

typedef struct ErnicOfiListener ErnicOfiListener;
typedef struct ErnicOfiWire ErnicOfiWire;

ErnicOfiListener *ernic_ofi_listener_open(uint16_t port);
void ernic_ofi_listener_close(ErnicOfiListener *lst);
ErnicOfiWire *ernic_ofi_listener_accept(ErnicOfiListener *lst, int timeout_us);

ErnicOfiWire *ernic_ofi_wire_connect(const char *host, uint16_t port);
void ernic_ofi_wire_close(ErnicOfiWire *w);

int ernic_ofi_wire_send_exact(ErnicOfiWire *w, const void *buf, size_t len);
int ernic_ofi_wire_recv_exact(ErnicOfiWire *w, void *buf, size_t len, int timeout_us);

int ernic_ofi_wire_send_framed(ErnicOfiWire *w, const TcpMsgHeader *hdr_net,
                               const void *payload, size_t payload_len);

int ernic_ofi_wire_recv_framed(ErnicOfiWire *w, TcpMsgHeader *hdr_net,
                               void **payload_out, int hdr_timeout_us);

void ernic_ofi_wire_free_framed_payload(void *payload);

int ernic_ofi_wire_send_eth_nonblock(ErnicOfiWire *w,
                                     const TcpMsgHeader *hdr_net,
                                     const void *payload, size_t payload_len);

#endif /* ERNIC_OFI_WIRE_H */
