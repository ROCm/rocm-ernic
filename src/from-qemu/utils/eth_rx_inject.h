/*
 * Ethernet RX Injection Utilities
 *
 * Functions to inject Ethernet frames into VM's RX descriptors.
 * Used by DHCP server and rdma_cm protocol handlers.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ETH_RX_INJECT_H
#define ETH_RX_INJECT_H

#include <stdint.h>
#include <stddef.h>

/* Forward declaration */
typedef struct PVRDMADev PVRDMADev;

/* Inject Ethernet frame into VM's RX descriptors
 * Returns: 0 on success, negative on error
 */
int eth_rx_inject_frame(PVRDMADev *dev, const void *frame_data, size_t len);

#endif /* ETH_RX_INJECT_H */
