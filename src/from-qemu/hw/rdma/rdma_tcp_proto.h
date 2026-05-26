/*
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * On-wire TCP mesh message header (shared by rdma_backend_tcp.c and
 * ernic_ofi_wire.c).  Must stay packed and identical to the historical
 * layout in rdma_backend_tcp.c.
 */

#ifndef RDMA_TCP_PROTO_H
#define RDMA_TCP_PROTO_H

#include <stdint.h>

typedef struct {
    uint32_t magic;
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t seq;
    uint32_t src_node_id;
    uint32_t dst_node_id;
    uint32_t src_qpn;
    uint32_t dst_qpn;
} __attribute__((packed)) TcpMsgHeader;

#endif /* RDMA_TCP_PROTO_H */
