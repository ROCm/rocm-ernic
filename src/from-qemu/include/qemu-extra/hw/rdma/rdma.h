/*
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_RDMA_RDMA_H
#define HW_RDMA_RDMA_H

#include <stdint.h>

/*
 * RDMA Backend Function Declarations
 * Note: These are forward declarations for functions implemented in QEMU source
 * files
 */

void rdma_backend_destroy(void *backend_dev);

/* DMA mapping functions - forward declarations */
void *rdma_pci_dma_map(void *dev, uint64_t addr, uint64_t len);
void rdma_pci_dma_unmap(void *dev, void *buffer, uint64_t len);

#endif /* HW_RDMA_RDMA_H */
