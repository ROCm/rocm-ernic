/*
 * ROCm ERNIC Ethernet Register Definitions
 *
 * Simple Ethernet support - Linux kernel handles TCP/IP on top.
 * These registers extend the existing PVRDMA register set.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ROCM_ERNIC_ETH_H
#define ROCM_ERNIC_ETH_H

#include <stdint.h>

/* Ethernet registers start at offset 0x28 (after PVRDMA_REG_MACH at 0x24) */
#define ROCM_ERNIC_ETH_CTL     0x28 /* Ethernet Control */
#define ROCM_ERNIC_ETH_STATUS  0x2c /* Ethernet Status */
#define ROCM_ERNIC_ETH_TX_BAL  0x30 /* Transmit Descriptor Base Low */
#define ROCM_ERNIC_ETH_TX_BAH  0x34 /* Transmit Descriptor Base High */
#define ROCM_ERNIC_ETH_TX_LEN  0x38 /* Transmit Descriptor Ring Length */
#define ROCM_ERNIC_ETH_TX_HEAD 0x3c /* Transmit Descriptor Head */
#define ROCM_ERNIC_ETH_TX_TAIL 0x40 /* Transmit Descriptor Tail */
#define ROCM_ERNIC_ETH_RX_BAL  0x44 /* Receive Descriptor Base Low */
#define ROCM_ERNIC_ETH_RX_BAH  0x48 /* Receive Descriptor Base High */
#define ROCM_ERNIC_ETH_RX_LEN  0x4c /* Receive Descriptor Ring Length */
#define ROCM_ERNIC_ETH_RX_HEAD 0x50 /* Receive Descriptor Head */
#define ROCM_ERNIC_ETH_RX_TAIL 0x54 /* Receive Descriptor Tail */
#define ROCM_ERNIC_ETH_ICR     0x58 /* Ethernet Interrupt Cause */
#define ROCM_ERNIC_ETH_IMR     0x5c /* Ethernet Interrupt Mask */

/* Ethernet Control Register bits */
#define ROCM_ERNIC_ETH_CTL_ENABLE    (1 << 0)  /* Enable Ethernet */
#define ROCM_ERNIC_ETH_CTL_TX_ENABLE (1 << 1)  /* Enable Transmit */
#define ROCM_ERNIC_ETH_CTL_RX_ENABLE (1 << 2)  /* Enable Receive */
#define ROCM_ERNIC_ETH_CTL_RESET     (1 << 31) /* Software Reset */

/* Ethernet Status Register bits */
#define ROCM_ERNIC_ETH_STATUS_LINK_UP (1 << 0) /* Link Up */
#define ROCM_ERNIC_ETH_STATUS_TX_BUSY (1 << 1) /* Transmit Busy */
#define ROCM_ERNIC_ETH_STATUS_RX_BUSY (1 << 2) /* Receive Busy */

/* Ethernet Interrupt Cause bits */
#define ROCM_ERNIC_ETH_ICR_TX_COMPLETE (1 << 0) /* Transmit Complete */
#define ROCM_ERNIC_ETH_ICR_RX_PACKET   (1 << 1) /* Receive Packet */
#define ROCM_ERNIC_ETH_ICR_TX_ERROR    (1 << 2) /* Transmit Error */
#define ROCM_ERNIC_ETH_ICR_RX_ERROR    (1 << 3) /* Receive Error */

/* Descriptor format (simplified - similar to e1000) */
struct rocm_ernic_eth_desc {
    uint64_t addr;    /* Buffer address (guest physical) */
    uint16_t length;  /* Buffer length */
    uint8_t status;   /* Status flags */
    uint8_t cmd;      /* Command flags */
    uint16_t vlan;    /* VLAN tag (unused for now) */
    uint8_t cso;      /* Checksum offset (unused for now) */
    uint8_t css;      /* Checksum start (unused for now) */
    uint16_t special; /* Special fields (unused for now) */
} __attribute__((packed));

/* Descriptor status bits */
#define ROCM_ERNIC_ETH_DESC_STATUS_DD (1 << 0) /* Descriptor Done */
#define ROCM_ERNIC_ETH_DESC_STATUS_EC (1 << 1) /* Excess Collisions */
#define ROCM_ERNIC_ETH_DESC_STATUS_LC (1 << 2) /* Late Collision */
#define ROCM_ERNIC_ETH_DESC_STATUS_TU (1 << 3) /* Transmit Underrun */
#define ROCM_ERNIC_ETH_DESC_STATUS_RS (1 << 4) /* Report Status */

/* Descriptor command bits */
#define ROCM_ERNIC_ETH_DESC_CMD_RS   (1 << 0) /* Report Status */
#define ROCM_ERNIC_ETH_DESC_CMD_IC   (1 << 1) /* Insert Checksum */
#define ROCM_ERNIC_ETH_DESC_CMD_IFCS (1 << 2) /* Insert FCS */
#define ROCM_ERNIC_ETH_DESC_CMD_EOP  (1 << 3) /* End of Packet */

#endif /* ROCM_ERNIC_ETH_H */
