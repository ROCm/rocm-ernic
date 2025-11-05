/*
 * Compatibility Bridge Implementation
 *
 * Implements the translation between QEMU and libvfio-user APIs.
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

/* Define this to get full QEMU headers */
#define VFU_PVRDMA_INTERNAL_IMPL

#include "vfu_compat_bridge.h"
#include "vfu_pvrdma_internal.h"
#include "from-qemu/hw/rdma/rdma_utils.h"

/*
 * DMA mapping implementation
 * 
 * In QEMU, this uses the AddressSpace API to map guest physical addresses.
 * In vfio-user, we use vfu_addr_to_sgl() and vfu_sgl_get() to map guest DMA regions.
 */

void *rdma_pci_dma_map(PCIDevice *pci_dev, dma_addr_t addr, size_t len)
{
    vfu_ctx_t *vfu_ctx = pci_dev->vfu_ctx;
    dma_sg_t *sg;
    struct iovec iov;
    void *buffer;
    int ret;
    
    if (!vfu_ctx || !addr || !len) {
        errno = EINVAL;
        return NULL;
    }
    
    /* Allocate scatter-gather entry */
    sg = alloca(dma_sg_size());
    if (!sg) {
        errno = ENOMEM;
        return NULL;
    }
    
    /* Convert guest DMA address to scatter-gather list */
    ret = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)(uintptr_t)addr, len, 
                          sg, 1, PROT_READ | PROT_WRITE);
    if (ret < 0) {
        rdma_error_report("Failed to convert DMA addr %#lx to SGL: %s",
                         addr, strerror(errno));
        return NULL;
    }
    
    /* Get host virtual address mapping */
    ret = vfu_sgl_get(vfu_ctx, sg, &iov, 1, 0);
    if (ret < 0) {
        rdma_error_report("Failed to get SGL mapping for addr %#lx: %s",
                         addr, strerror(errno));
        return NULL;
    }
    
    if (iov.iov_len < len) {
        rdma_warn_report("DMA mapping shorter than requested: got %zu, wanted %zu",
                        iov.iov_len, len);
        /* Still usable, but caller should be aware */
    }
    
    buffer = iov.iov_base;
    
    /* Store the SGL for later unmapping - we'll need a mapping table */
    /* For now, just return the buffer */
    return buffer;
}

void rdma_pci_dma_unmap(PCIDevice *pci_dev, void *buffer, size_t len)
{
    vfu_ctx_t *vfu_ctx = pci_dev->vfu_ctx;
    dma_sg_t *sg;
    struct iovec iov;
    
    if (!vfu_ctx || !buffer) {
        return;
    }
    
    /* TODO: Look up the SGL from a mapping table */
    /* For now, we'll just release directly */
    
    sg = alloca(dma_sg_size());
    if (!sg) {
        return;
    }
    
    /* Reconstruct iov for unmapping */
    iov.iov_base = buffer;
    iov.iov_len = len;
    
    /* Release the mapping */
    vfu_sgl_put(vfu_ctx, sg, &iov, 1);
}

/*
 * Interrupt handling
 */

void post_interrupt(void *dev, unsigned vector)
{
    PVRDMADev *pvrdma = (PVRDMADev *)dev;
    vfu_pvrdma_dev_t *vfu_dev = (vfu_pvrdma_dev_t *)((char *)pvrdma - 
                                                       offsetof(vfu_pvrdma_dev_t, pvrdma));
    vfu_ctx_t *vfu_ctx = vfu_dev->vfu_ctx;
    int ret;
    
    if (!vfu_ctx) {
        rdma_error_report("post_interrupt: vfu_ctx is NULL");
        return;
    }
    
    if (vector >= RDMA_MAX_INTRS) {
        rdma_error_report("post_interrupt: invalid vector %u", vector);
        return;
    }
    
    /* Trigger MSI-X interrupt */
    ret = vfu_irq_trigger(vfu_ctx, vector);
    if (ret < 0) {
        rdma_error_report("Failed to trigger interrupt vector %u: %s",
                         vector, strerror(errno));
        return;
    }
    
    pvrdma->stats.interrupts++;
    
    rdma_debug_report("Triggered interrupt vector %u", vector);
}

