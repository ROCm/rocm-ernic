/*
 * Ethernet RX Injection Implementation
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "eth_rx_inject.h"
#include "from-qemu/hw/rdma/vmw/pvrdma_eth.h"
#include "rocm_ernic_eth.h"
#include "from-qemu/hw/rdma/rdma_utils.h"
#include "hw/rdma/rdma.h" /* For rdma_pci_dma_map/unmap */
#include "hw/pci/pci.h"   /* For PCIDevice and pci_dma_sync */
#include <string.h>
#include <inttypes.h>

int eth_rx_inject_frame(PVRDMADev *dev, const void *frame_data, size_t len)
{
    PVRDMAEthState *eth = get_eth_state(dev);

    if (!eth || !frame_data || len == 0) {
        return -EINVAL;
    }

    if (!(eth->ctl & ROCM_ERNIC_ETH_CTL_RX_ENABLE)) {
        rdma_warn_report("Ethernet RX not enabled");
        return -EINVAL;
    }

    if (eth->rx_base == 0 || eth->rx_len == 0) {
        rdma_warn_report("RX descriptors not initialized");
        return -EINVAL;
    }

    /* Find next available RX descriptor */
    uint32_t next_tail = (eth->rx_tail + 1) % eth->rx_len;
    if (next_tail == eth->rx_head) {
        rdma_warn_report("RX descriptor ring full");
        return -ENOSPC;
    }

    PCIDevice *pci_dev = &dev->parent_obj;
    uint32_t desc_idx = eth->rx_tail;
    uint64_t desc_addr =
        eth->rx_base + (desc_idx * sizeof(struct rocm_ernic_eth_desc));

    /* Map descriptor */
    void *desc_vaddr = rdma_pci_dma_map(pci_dev, desc_addr,
                                        sizeof(struct rocm_ernic_eth_desc));
    if (!desc_vaddr) {
        rdma_error_report("Failed to map RX descriptor at 0x%" PRIx64,
                          desc_addr);
        return -EFAULT;
    }

    struct rocm_ernic_eth_desc desc;
    memcpy(&desc, desc_vaddr, sizeof(desc));

    /* Check if descriptor buffer is large enough */
    if (desc.length < len) {
        rdma_error_report("RX descriptor buffer too small (%u < %zu)",
                          desc.length, len);
        rdma_pci_dma_unmap(pci_dev, desc_vaddr, sizeof(desc));
        return -ENOSPC;
    }

    /* Map packet buffer */
    void *packet_vaddr = rdma_pci_dma_map(pci_dev, desc.addr, len);
    if (!packet_vaddr) {
        rdma_error_report("Failed to map RX packet buffer at 0x%" PRIx64,
                          desc.addr);
        rdma_pci_dma_unmap(pci_dev, desc_vaddr, sizeof(desc));
        return -EFAULT;
    }

    /* Copy frame data to packet buffer */
    memcpy(packet_vaddr, frame_data, len);

    /* Sync DMA write */
    pci_dma_sync(pci_dev, desc.addr, len);

    /* Update descriptor */
    desc.length = len;
    desc.status |= ROCM_ERNIC_ETH_DESC_STATUS_DD;
    desc.status |= ROCM_ERNIC_ETH_DESC_STATUS_RS; /* Report Status */
    memcpy(desc_vaddr, &desc, sizeof(desc));

    /* Sync descriptor write */
    pci_dma_sync(pci_dev, desc_addr, sizeof(desc));

    /* Unmap buffers */
    rdma_pci_dma_unmap(pci_dev, packet_vaddr, len);
    rdma_pci_dma_unmap(pci_dev, desc_vaddr, sizeof(desc));

    /* Advance tail pointer */
    eth->rx_tail = next_tail;
    dev->stats.total_ip_bytes_rx += len;

    /* Set interrupt */
    eth->icr |= ROCM_ERNIC_ETH_ICR_RX_PACKET;
    if (eth->imr & ROCM_ERNIC_ETH_ICR_RX_PACKET) {
        post_interrupt(dev, INTR_VEC_CMD_RING);
    }

    /* Hot path -- no logging here */

    return 0;
}
