/*
 * ROCm ERNIC Ethernet Support Header
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PVRDMA_ETH_H
#define PVRDMA_ETH_H

#include "pvrdma.h"
#include <stdint.h>

/* Ethernet state structure */
typedef struct {
    uint32_t ctl;     /* Control register */
    uint32_t status;  /* Status register */
    uint64_t tx_base; /* Transmit descriptor base */
    uint32_t tx_len;  /* Transmit descriptor ring length */
    uint32_t tx_head; /* Transmit descriptor head */
    uint32_t tx_tail; /* Transmit descriptor tail */
    uint64_t rx_base; /* Receive descriptor base */
    uint32_t rx_len;  /* Receive descriptor ring length */
    uint32_t rx_head; /* Receive descriptor head */
    uint32_t rx_tail; /* Receive descriptor tail */
    uint32_t icr;     /* Interrupt cause register */
    uint32_t imr;     /* Interrupt mask register */
} PVRDMAEthState;

/* Get Ethernet state from PVRDMA device */
PVRDMAEthState *get_eth_state(PVRDMADev *dev);

/* Ethernet register handlers */
uint64_t pvrdma_eth_regs_read(PVRDMADev *dev, hwaddr addr);
void pvrdma_eth_regs_write(PVRDMADev *dev, hwaddr addr, uint64_t val);
void pvrdma_eth_process_tx(PVRDMADev *dev);
void pvrdma_eth_rx_frame(PVRDMADev *dev, const void *frame_data, size_t len);

#endif /* PVRDMA_ETH_H */
