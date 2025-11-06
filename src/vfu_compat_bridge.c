/*
 * Compatibility Bridge Implementation
 *
 * This file implements the wrapper functions that isolate QEMU code from
 * our libvfio-user server. Only this file includes QEMU headers.
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <inttypes.h> /* For PRId64, PRIx64 */
#include <sys/mman.h> /* For PROT_READ, PROT_WRITE */

/* Define libvfio-user types we need - full structs to avoid incomplete type */
#include <vfio-user/libvfio-user.h>

#include "vfu_compat_bridge.h"
#include "vfu_pvrdma_internal.h"

/* Now we can include QEMU headers */
#define VFU_PVRDMA_INTERNAL_IMPL
#include "from-qemu/hw/rdma/vmw/pvrdma.h"
#include "from-qemu/hw/rdma/rdma_backend.h"
#include "from-qemu/hw/rdma/rdma_utils.h"

/*
 * Device Management
 */

pvrdma_handle_t pvrdma_device_create(vfu_pvrdma_dev_t *vfu_dev,
                                     const char *ib_dev_name,
                                     const char *eth_dev_name, uint8_t port_num)
{
    PVRDMADev *pvrdma;

    if (!vfu_dev) {
        rdma_error_report("pvrdma_device_create: vfu_dev is NULL");
        return NULL;
    }

    /* Allocate QEMU PVRDMA device structure */
    pvrdma = calloc(1, sizeof(PVRDMADev));
    if (!pvrdma) {
        rdma_error_report("Failed to allocate PVRDMADev");
        return NULL;
    }

    /* Initialize PCIDevice wrapper for bridge functions */
    pvrdma->parent_obj.vfu_dev = vfu_dev;
    pvrdma->parent_obj.vfu_ctx = vfu_dev->vfu_ctx;

    /* Set backend device configuration */
    if (ib_dev_name) {
        pvrdma->backend_device_name = strdup(ib_dev_name);
    }
    if (eth_dev_name) {
        pvrdma->backend_eth_device_name = strdup(eth_dev_name);
    }
    pvrdma->backend_port_num = port_num;

    /* Initialize device attributes with defaults */
    pvrdma->dev_attr.max_qp = MAX_QP;
    pvrdma->dev_attr.max_cq = MAX_CQ;
    pvrdma->dev_attr.max_mr = MAX_MR;
    pvrdma->dev_attr.max_pd = MAX_PD;
    pvrdma->dev_attr.max_qp_rd_atom = MAX_QP_RD_ATOM;
    pvrdma->dev_attr.max_qp_init_rd_atom = MAX_QP_INIT_RD_ATOM;
    pvrdma->dev_attr.max_ah = MAX_AH;
    pvrdma->dev_attr.max_srq = MAX_SRQ;
    pvrdma->dev_attr.max_mr_size = MAX_MR_SIZE;

    /* Initialize DSR info */
    pvrdma->dsr_info.dsr = NULL;
    pvrdma->dsr_info.dma = 0;

    /* Initialize stats */
    memset(&pvrdma->stats, 0, sizeof(pvrdma->stats));

    /* Set interrupt mask to 0 (interrupts enabled) */
    pvrdma->interrupt_mask = 0;

    rdma_info_report("PVRDMA device created (handle=%p)", pvrdma);

    return (pvrdma_handle_t)pvrdma;
}

void pvrdma_device_destroy(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;

    if (!pvrdma) {
        return;
    }

    /* Clean up backend */
    rdma_backend_destroy(&pvrdma->backend_dev);
    rdma_rm_fini(&pvrdma->rdma_dev_res);

    /* Free device names */
    free(pvrdma->backend_device_name);
    free(pvrdma->backend_eth_device_name);

    /* Free DSR if allocated */
    if (pvrdma->dsr_info.dsr) {
        free(pvrdma->dsr_info.dsr);
    }

    free(pvrdma);

    rdma_info_report("PVRDMA device destroyed");
}

int pvrdma_device_realize(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;
    int rc;

    if (!pvrdma) {
        return -EINVAL;
    }

    /* Initialize registers - must be done before driver probes */
    pvrdma->regs_data[0] = PVRDMA_HW_VERSION;  /* PVRDMA_REG_VERSION at offset 0 */
    pvrdma->regs_data[1] = 0xFFFF;              /* PVRDMA_REG_ERR */
    
    rdma_info_report("PVRDMA version register initialized to %d", PVRDMA_HW_VERSION);

    /* Initialize resource manager */
    if (rdma_rm_init(&pvrdma->rdma_dev_res, &pvrdma->dev_attr) < 0) {
        rdma_error_report("Failed to initialize resource manager");
        return -EIO;
    }

    /* Initialize RDMA backend - optional for now */
    rc = rdma_backend_init(
        &pvrdma->backend_dev, &pvrdma->rdma_dev_res,
        pvrdma->backend_device_name, pvrdma->backend_eth_device_name,
        pvrdma->backend_port_num, &pvrdma->dev_attr, NULL); /* No MAD chr_be */

    if (rc < 0) {
        rdma_warn_report("RDMA backend initialization failed (rc=%d) - continuing without backend", rc);
        rdma_warn_report("Device will respond to PCI/register accesses but RDMA operations won't work");
        /* Don't return error - allow device to realize for testing */
    } else {
        rdma_info_report("RDMA backend initialized successfully");
    }

    rdma_info_report("PVRDMA device realized successfully");

    return 0;
}

/*
 * Register Access (BAR1)
 */

void pvrdma_regs_write(pvrdma_handle_t handle, hwaddr offset, uint32_t value,
                       unsigned size)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;

    if (!pvrdma) {
        return;
    }

    /* Forward to QEMU register write implementation */
    pvrdma_regs_write_impl(pvrdma, offset, value, size);
}

uint32_t pvrdma_regs_read(pvrdma_handle_t handle, hwaddr offset, unsigned size)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;
    uint32_t val = 0;

    if (!pvrdma) {
        return 0;
    }

    /* Forward to QEMU register read implementation */
    val = pvrdma_regs_read_impl(pvrdma, offset, size);

    return val;
}

/*
 * UAR Access (BAR2)
 */

void pvrdma_uar_write(pvrdma_handle_t handle, hwaddr offset, uint32_t value,
                      unsigned size)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;

    if (!pvrdma) {
        return;
    }

    /* Forward to QEMU UAR write implementation */
    pvrdma_uar_write_impl(pvrdma, offset, value, size);
}

uint32_t pvrdma_uar_read(pvrdma_handle_t handle, hwaddr offset, unsigned size)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;
    uint32_t val = 0;

    if (!pvrdma) {
        return 0;
    }

    /* Forward to QEMU UAR read implementation */
    val = pvrdma_uar_read_impl(pvrdma, offset, size);

    return val;
}

/*
 * Command Execution - pvrdma_exec_cmd is implemented in pvrdma_cmd.c
 */

/*
 * Statistics
 */

void pvrdma_get_stats(pvrdma_handle_t handle, uint64_t *commands,
                      uint64_t *regs_reads, uint64_t *regs_writes,
                      uint64_t *uar_writes, uint64_t *interrupts)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;

    if (!pvrdma) {
        return;
    }

    if (commands)
        *commands = pvrdma->stats.commands;
    if (regs_reads)
        *regs_reads = pvrdma->stats.regs_reads;
    if (regs_writes)
        *regs_writes = pvrdma->stats.regs_writes;
    if (uar_writes)
        *uar_writes = pvrdma->stats.uar_writes;
    if (interrupts)
        *interrupts = pvrdma->stats.interrupts;
}

/*
 * DMA Mapping (called FROM QEMU code)
 */

/* Implement pci_dma_map - called by QEMU PVRDMA code via hw/pci/pci.h */
void *pci_dma_map(PCIDevice *dev, dma_addr_t addr, dma_addr_t *plen, int dir)
{
    vfu_ctx_t *vfu_ctx;
    dma_sg_t *sg;
    struct iovec iov;
    void *host_addr;
    int ret;
    size_t sg_size;

    rdma_info_report("=== DMA MAP CALLED ===");
    rdma_info_report("  guest_addr=%#lx requested_len=%zu dir=%d", 
                     addr, plen ? (size_t)*plen : 0, dir);

    if (!dev) {
        rdma_error_report("DMA map: NULL device pointer");
        if (plen) *plen = 0;
        return NULL;
    }
    
    rdma_info_report("  dev=%p", dev);
    
    if (!dev->vfu_ctx) {
        rdma_error_report("DMA map: NULL vfu_ctx in PCIDevice (dev=%p)", dev);
        rdma_error_report("  This means vfu_ctx was not set in pvrdma_device_create");
        if (plen) *plen = 0;
        return NULL;
    }
    
    if (!plen) {
        rdma_error_report("DMA map: NULL plen pointer");
        return NULL;
    }

    vfu_ctx = dev->vfu_ctx;
    rdma_info_report("  vfu_ctx=%p", vfu_ctx);

    (void)dir; /* Direction not used with vfio-user */

    /* Get SG size */
    sg_size = dma_sg_size();
    rdma_info_report("  Allocating SG of size %zu", sg_size);
    
    /* Allocate scatter-gather entry */
    sg = malloc(sg_size);
    if (!sg) {
        rdma_error_report("DMA map: Failed to allocate SG entry of size %zu", sg_size);
        *plen = 0;
        return NULL;
    }

    rdma_info_report("  Calling vfu_addr_to_sgl(ctx=%p, addr=%#lx, len=%zu, sg=%p, cnt=1, prot=RW)",
                     vfu_ctx, addr, (size_t)*plen, sg);

    /* Convert guest physical address to scatter-gather list */
    ret = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)(uintptr_t)addr, *plen, sg,
                          1, PROT_READ | PROT_WRITE);
    if (ret < 0) {
        rdma_error_report("DMA map: vfu_addr_to_sgl FAILED for addr=%#lx: %s (errno=%d)", 
                          addr, strerror(errno), errno);
        rdma_error_report("  This usually means the guest address is not in any registered DMA region");
        free(sg);
        *plen = 0;
        return NULL;
    }
    
    rdma_info_report("  vfu_addr_to_sgl returned %d (success)", ret);

    rdma_info_report("  Calling vfu_sgl_get(ctx=%p, sg=%p, iov=%p, cnt=1)",
                     vfu_ctx, sg, &iov);

    /* Get host virtual address */
    ret = vfu_sgl_get(vfu_ctx, sg, &iov, 1, 0);
    if (ret < 0) {
        rdma_error_report("DMA map: vfu_sgl_get FAILED for addr=%#lx: %s (errno=%d)", 
                          addr, strerror(errno), errno);
        rdma_error_report("  This usually means the memory is not mapped (vaddr=NULL in DMA region)");
        free(sg);
        *plen = 0;
        return NULL;
    }
    
    rdma_info_report("  vfu_sgl_get returned %d (success)", ret);

    host_addr = iov.iov_base;
    *plen = iov.iov_len; /* Update with actual mapped length */
    
    rdma_info_report("  iov.iov_base=%p iov.iov_len=%zu", host_addr, iov.iov_len);

    /* Free SG - we only needed it for mapping */
    free(sg);

    if (!host_addr) {
        rdma_error_report("DMA map: vfu_sgl_get returned NULL iov_base for guest=%#lx", addr);
        rdma_error_report("  This means the region is not memory-mapped");
        *plen = 0;
        return NULL;
    }

    rdma_info_report("=== DMA MAP SUCCESS: guest=%#lx -> host=%p len=%zu ===", 
                     addr, host_addr, (size_t)*plen);

    return host_addr;
}

/* Implement pci_dma_unmap - called by QEMU PVRDMA code via hw/pci/pci.h */
void pci_dma_unmap(PCIDevice *dev, void *buffer, dma_addr_t len, int dir,
                   dma_addr_t access_len)
{
    (void)dev;
    (void)buffer;
    (void)len;
    (void)dir;
    (void)access_len;

    /* For unmapping, we just need to tell libvfio-user about the buffer.
     * Since we don't track the original sg from the map call, we can't
     * properly call vfu_sgl_put. This is a limitation of the current
     * pci_dma_unmap API which doesn't give us the original mapping info.
     *
     * For now, we'll skip the unmap - libvfio-user will handle cleanup
     * when regions are unmapped or the device is destroyed.
     */

    rdma_debug_report("DMA unmap: host=%p len=%zu (no-op)", buffer,
                      (size_t)len);
}

/*
 * Interrupt Handling (called FROM QEMU code)
 */

void post_interrupt(PVRDMADev *pvrdma, unsigned vector)
{
    vfu_pvrdma_dev_t *vfu_dev;
    vfu_ctx_t *vfu_ctx;
    int ret;

    if (!pvrdma || !pvrdma->parent_obj.vfu_dev || !pvrdma->parent_obj.vfu_ctx) {
        rdma_error_report("post_interrupt: invalid device pointer");
        return;
    }

    vfu_dev = pvrdma->parent_obj.vfu_dev;
    vfu_ctx = pvrdma->parent_obj.vfu_ctx;

    if (vector >= RDMA_MAX_INTRS) {
        rdma_error_report("post_interrupt: invalid vector %u", vector);
        return;
    }

    /* Check interrupt mask */
    if (pvrdma->interrupt_mask) {
        rdma_debug_report("Interrupt vector %u masked", vector);
        return;
    }

    /* Trigger MSI-X interrupt via libvfio-user */
    ret = vfu_irq_trigger(vfu_ctx, vector);
    if (ret < 0) {
        rdma_error_report("Failed to trigger interrupt vector %u: %s", vector,
                          strerror(errno));
        return;
    }

    pvrdma->stats.interrupts++;

    rdma_debug_report("Triggered interrupt vector %u", vector);
}
