/*
 * rdma_cm Protocol Handler (loopback mode only)
 *
 * Stub CM responder for loopback / single-VM mode.
 * Not used by the TCP mesh backend; see pvrdma_eth.c
 * for the forwarding path used in multi-VM mode.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef RDMA_CM_PROTO_H
#define RDMA_CM_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Process rdma_cm message from TCP payload
 * Returns: response size, or 0 if no response needed
 */
size_t rdma_cm_process_message(const void *tcp_payload, size_t payload_len,
                               void *response, size_t max_response_len);

#endif /* RDMA_CM_PROTO_H */
