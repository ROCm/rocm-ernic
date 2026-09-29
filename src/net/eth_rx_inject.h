/*
 * Ethernet RX Injection Utilities
 *
 * Functions to inject Ethernet frames into VM's RX descriptors.
 * Used by DHCP server and rdma_cm protocol handlers.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ETH_RX_INJECT_H
#define ETH_RX_INJECT_H

#include <stdint.h>
#include <stddef.h>

/* Forward declaration */
typedef struct PVRDMADev PVRDMADev;

/*
 * Injects an Ethernet frame into the guest RX descriptors, blocking the
 * caller until the frame is queued in the RX ring or a non-recoverable
 * error occurs.  Retries while the guest RX ring
 * is full (-ENOSPC) with short sleeps so the vCPU can post buffers, which
 * applies TCP backpressure because the mesh TCP recv thread does not read
 * further messages until this returns.
 */
int eth_rx_inject_frame_mesh_blocking(PVRDMADev *dev, const void *frame_data,
                                      size_t len);

#endif /* ETH_RX_INJECT_H */
