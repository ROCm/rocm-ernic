/*
 * Compatibility Bridge Implementation
 *
 * This file implements the wrapper functions that isolate QEMU code from
 * our libvfio-user server. Only this file includes QEMU headers.
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <inttypes.h>  /* For PRId64, PRIx64 */
#include <sys/mman.h>  /* For PROT_READ, PROT_WRITE */
#include <arpa/inet.h> /* For inet_addr */

/* Define libvfio-user types we need - full structs to avoid incomplete type */
#include <vfio-user/libvfio-user.h>

#include "rocm_ernic_compat.h"
#include "rocm_ernic_internal.h"

/*
 * QEMU headers -- suppress warnings from upstream code
 * that we do not own.  This is the standard pattern for
 * third-party header inclusions.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wshift-overflow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wpacked"
#include "from-qemu/hw/rdma/vmw/pvrdma.h"
#include "from-qemu/hw/rdma/vmw/pvrdma_qp_ops.h"
#include "from-qemu/hw/rdma/rdma_backend.h"
#include "from-qemu/hw/rdma/rdma_backend_ops.h"
#include "from-qemu/hw/rdma/rdma_rm.h"
#include "from-qemu/hw/rdma/rdma_utils.h"
#include "from-qemu/utils/dhcp_server.h"
#include "from-qemu/utils/dhcp_proxy.h"
#include "from-qemu/include/qemu-extra/standard-headers/rdma/vmw_pvrdma-abi.h"
#include "from-qemu/include/qemu-extra/standard-headers/drivers/infiniband/hw/vmw_pvrdma/pvrdma_dev_api.h"
#include "from-qemu/include/qemu-extra/hw/pci/pci.h"
#pragma GCC diagnostic pop

/* Forward declaration -- defined later in this file,
 * also referenced via extern in pvrdma_main.c. */
void pvrdma_dsr_flush(void *handle);

/*
 * DMA Mapping Tracking
 * We must track SGL/iovec pairs to properly call vfu_sgl_put()
 */
typedef struct dma_mapping {
    dma_addr_t guest_addr;
    size_t len;
    dma_sg_t *sg;
    struct iovec iov;
    void *host_addr;
    vfu_ctx_t *vfu_ctx;
} dma_mapping_t;

#define MAX_DMA_MAPPINGS 256
static dma_mapping_t dma_mappings[MAX_DMA_MAPPINGS];
static int num_dma_mappings = 0;

/*
 * Device Management
 */

pvrdma_handle_t pvrdma_device_create(rocm_ernic_dev_t *dev,
                                     const char *backend_type_str,
                                     const char *ib_dev_name,
                                     const char *eth_dev_name, uint8_t port_num)
{
    PVRDMADev *pvrdma;
    RdmaBackendType backend_type;

    if (!dev) {
        rdma_error_report("pvrdma_device_create: dev is NULL");
        return NULL;
    }

    /* Allocate QEMU PVRDMA device structure */
    pvrdma = calloc(1, sizeof(PVRDMADev));
    if (!pvrdma) {
        rdma_error_report("Failed to allocate PVRDMADev");
        return NULL;
    }

    /* Initialize PCIDevice wrapper for bridge functions */
    pvrdma->parent_obj.vfu_dev = dev;
    pvrdma->parent_obj.vfu_ctx = dev->vfu_ctx;

    /* Parse backend type and extract config */
    backend_type = rdma_backend_get_type_from_string(backend_type_str);
    rdma_info_report("Selected RDMA backend: %s",
                     rdma_backend_type_to_string(backend_type));

    /* Store backend type and config in device for later use */
    pvrdma->backend_dev.backend_type = backend_type;

    /* Extract backend config (part after ':') from backend string */
    const char *colon = strchr(backend_type_str, ':');
    const char *backend_config_from_string = colon ? (colon + 1) : NULL;

    /* Set backend device configuration based on backend type */
    if (backend_type == RDMA_BACKEND_TYPE_VERBS) {
        /* For verbs backend, prefer explicit ib_dev_name, fallback to config
         * string */
        if (ib_dev_name) {
            pvrdma->backend_device_name = strdup(ib_dev_name);
        } else if (backend_config_from_string) {
            pvrdma->backend_device_name = strdup(backend_config_from_string);
        }
    } else if (backend_config_from_string) {
        /* For other backends (loopback, etc), use config from backend string */
        pvrdma->backend_device_name = strdup(backend_config_from_string);
        rdma_info_report("Backend config string: '%s'",
                         pvrdma->backend_device_name);
    }

    if (eth_dev_name) {
        pvrdma->backend_eth_device_name = strdup(eth_dev_name);
    }
    pvrdma->backend_port_num = port_num;

    /* Copy MAC address from rocm_ernic_dev */
    memcpy(pvrdma->mac_addr, dev->mac_addr, 6);
    pvrdma->mac_addr_set = dev->mac_addr_set;

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
    pvrdma->dev_attr.max_sge = MAX_SGE; /* Required for calculations below */

    /* Calculate dynamic device capabilities (from init_dev_caps in
     * pvrdma_main.c) */
    {
        size_t pg_tbl_bytes = 2 * PAGE_SIZE * (PAGE_SIZE / sizeof(uint64_t));
        size_t wr_sz = MAX(sizeof(struct pvrdma_sq_wqe_hdr),
                           sizeof(struct pvrdma_rq_wqe_hdr));

        rdma_info_report("Calculating device capabilities:");
        rdma_info_report("  PAGE_SIZE=%zu, pg_tbl_bytes=%zu", (size_t)PAGE_SIZE,
                         pg_tbl_bytes);

        /* Calculate max_qp_wr */
        pvrdma->dev_attr.max_qp_wr =
            (int)(pg_tbl_bytes /
                      (wr_sz + sizeof(struct pvrdma_sge) *
                                   (size_t)pvrdma->dev_attr.max_sge) -
                  PAGE_SIZE);

        /* Calculate max_cqe */
        pvrdma->dev_attr.max_cqe =
            (int)(pg_tbl_bytes / sizeof(struct pvrdma_cqe) - PAGE_SIZE);

        /* Calculate max_srq_wr */
        pvrdma->dev_attr.max_srq_wr =
            (int)(pg_tbl_bytes / ((sizeof(struct pvrdma_rq_wqe_hdr) +
                                   sizeof(struct pvrdma_sge)) *
                                  (size_t)pvrdma->dev_attr.max_sge) -
                  PAGE_SIZE);

        rdma_info_report("  max_qp_wr=%d", pvrdma->dev_attr.max_qp_wr);
        rdma_info_report("  max_cqe=%d", pvrdma->dev_attr.max_cqe);
        rdma_info_report("  max_sge=%d", pvrdma->dev_attr.max_sge);
        rdma_info_report("  max_srq_wr=%d", pvrdma->dev_attr.max_srq_wr);
    }

    /* Initialize DSR info */
    pvrdma->dsr_info.dsr = NULL;
    pvrdma->dsr_info.dma = 0;

    /* Initialize stats */
    memset(&pvrdma->stats, 0, sizeof(pvrdma->stats));
    pvrdma->stats.qp_stats =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    pvrdma->stats.stats_file = NULL;
    pvrdma->stats.stats_fp = NULL;
    pvrdma->stats.stats_write_count = 0;

    /* Set interrupt mask to 0 (interrupts enabled) */
    pvrdma->interrupt_mask = 0;

    rdma_info_report("PVRDMA device created (handle=%p)", pvrdma);

    return (pvrdma_handle_t)pvrdma;
}

/* Forward declaration */
void free_dsr(PVRDMADev *dev);

void pvrdma_device_destroy(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;

    if (!pvrdma) {
        return;
    }

    /* Clean up resource manager */
    rdma_rm_fini(&pvrdma->rdma_dev_res, &pvrdma->backend_dev,
                 pvrdma->backend_eth_device_name);

    /* Clean up backend using the new abstraction */
    rdma_backend_fini_with_ops(&pvrdma->backend_dev);

    /* Free stats hash table */
    if (pvrdma->stats.qp_stats) {
        g_hash_table_destroy(pvrdma->stats.qp_stats);
        pvrdma->stats.qp_stats = NULL;
    }
    if (pvrdma->stats.stats_file) {
        free(pvrdma->stats.stats_file);
        pvrdma->stats.stats_file = NULL;
    }
    free(pvrdma->stats_socket_path);
    pvrdma->stats_socket_path = NULL;
    free(pvrdma->stats_backend_str);
    pvrdma->stats_backend_str = NULL;
    free(pvrdma->stats_connection_str);
    pvrdma->stats_connection_str = NULL;
    if (pvrdma->stats.stats_fp) {
        fclose(pvrdma->stats.stats_fp);
        pvrdma->stats.stats_fp = NULL;
    }

    /* Free DSR and all DMA mappings properly */
    free_dsr(pvrdma);

    /* Free device names */
    free(pvrdma->backend_device_name);
    free(pvrdma->backend_eth_device_name);

    /* Free TCP connections */
    if (pvrdma->tcp_connections) {
        g_hash_table_destroy(pvrdma->tcp_connections);
        pvrdma->tcp_connections = NULL;
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
    /* Use set_reg_val() for proper address-to-index conversion */
    set_reg_val(pvrdma, PVRDMA_REG_VERSION, PVRDMA_HW_VERSION);
    set_reg_val(pvrdma, PVRDMA_REG_ERR, 0xFFFF);

    rdma_info_report("PVRDMA version register initialized to %d",
                     PVRDMA_HW_VERSION);

    /* Initialize RDMA backend with selected backend type */
    const char *backend_config =
        pvrdma
            ->backend_device_name; /* Config extracted from --backend string */

    rc = rdma_backend_init_with_ops(
        &pvrdma->backend_dev, pvrdma->backend_dev.backend_type, backend_config);

    if (rc < 0) {
        rdma_error_report("RDMA backend initialization failed (rc=%d)", rc);
        rdma_error_report(
            "Backend type: %s",
            rdma_backend_type_to_string(pvrdma->backend_dev.backend_type));
        return -EIO;
    }

    /* CRITICAL: Link backend_dev to the device resources */
    pvrdma->backend_dev.rdma_dev_res = &pvrdma->rdma_dev_res;
    rdma_info_report("Linked backend_dev to rdma_dev_res at %p",
                     pvrdma->backend_dev.rdma_dev_res);

    /* CRITICAL: Set PCIDevice pointer for DMA operations */
    /* PVRDMADev has PCIDevice parent_obj as first field, so we can cast */
    pvrdma->backend_dev.dev = (PCIDevice *)pvrdma;
    rdma_info_report("Set backend_dev->dev to PCIDevice at %p",
                     pvrdma->backend_dev.dev);

    /* Initialize DHCP server for loopback mode and TCP manager mode */
    if (pvrdma->backend_dev.backend_type == RDMA_BACKEND_TYPE_LOOPBACK) {
        /* Default DHCP configuration: 192.168.100.0/24 */
        uint32_t server_ip = inet_addr("192.168.100.1");
        uint32_t subnet_mask = inet_addr("255.255.255.0");
        uint32_t router_ip = inet_addr("192.168.100.1");
        uint32_t dns_server = inet_addr("192.168.100.1");
        uint32_t ip_pool_start = inet_addr("192.168.100.10");
        uint32_t ip_pool_end = inet_addr("192.168.100.254");
        uint32_t lease_time = 3600; /* 1 hour */

        pvrdma->dhcp_server =
            dhcp_server_create(server_ip, subnet_mask, router_ip, dns_server,
                               ip_pool_start, ip_pool_end, lease_time);
        if (!pvrdma->dhcp_server) {
            rdma_error_report("Failed to create DHCP server");
            return -ENOMEM;
        }
        rdma_info_report("DHCP server initialized for loopback mode");
    } else if (pvrdma->backend_dev.backend_type == RDMA_BACKEND_TYPE_TCP &&
               backend_config && strstr(backend_config, "manager:")) {
        /* TCP manager mode: Initialize DHCP server for allocating addresses
         * to netdev devices in VMs on connecting worker servers */
        /* Default DHCP configuration: 192.168.100.0/24 */
        uint32_t server_ip = inet_addr("192.168.100.1");
        uint32_t subnet_mask = inet_addr("255.255.255.0");
        uint32_t router_ip = inet_addr("192.168.100.1");
        uint32_t dns_server = inet_addr("192.168.100.1");
        uint32_t ip_pool_start = inet_addr("192.168.100.10");
        uint32_t ip_pool_end = inet_addr("192.168.100.254");
        uint32_t lease_time = 3600; /* 1 hour */

        pvrdma->dhcp_server =
            dhcp_server_create(server_ip, subnet_mask, router_ip, dns_server,
                               ip_pool_start, ip_pool_end, lease_time);
        if (!pvrdma->dhcp_server) {
            rdma_error_report("Failed to create DHCP server");
            return -ENOMEM;
        }
        rdma_info_report("DHCP server initialized for TCP manager mode");
    }
    /* Note: DHCP proxy for TCP worker mode is initialized lazily in
     * pvrdma_eth.c when the first DHCP request is received, since the manager
     * connection may not be ready at device realize time */

    /* Query device capabilities from backend to populate dev_attr */
    if (pvrdma->backend_dev.backend_ops &&
        pvrdma->backend_dev.backend_ops->query_device) {
        rc = pvrdma->backend_dev.backend_ops->query_device(&pvrdma->backend_dev,
                                                           &pvrdma->dev_attr);
        if (rc < 0) {
            rdma_error_report("Backend query_device failed (rc=%d)", rc);
            return -EIO;
        }
        rdma_info_report("Backend device attributes queried: max_qp=%d, "
                         "max_cq=%d, max_pd=%d",
                         pvrdma->dev_attr.max_qp, pvrdma->dev_attr.max_cq,
                         pvrdma->dev_attr.max_pd);
    } else {
        rdma_error_report("Backend does not support query_device");
        return -ENOTSUP;
    }

    rdma_info_report(
        "RDMA backend '%s' initialized successfully",
        rdma_backend_type_to_string(pvrdma->backend_dev.backend_type));

    /* Initialize resource manager AFTER querying device capabilities */
    rdma_info_report("pvrdma_device_realize: About to call rdma_rm_init, "
                     "pvrdma=%p, &pvrdma->rdma_dev_res=%p",
                     pvrdma, &pvrdma->rdma_dev_res);
    if (rdma_rm_init(&pvrdma->rdma_dev_res, &pvrdma->dev_attr) < 0) {
        rdma_error_report("Failed to initialize resource manager");
        return -EIO;
    }
    rdma_info_report("Resource manager initialized with dev_attr tables");

    /* Initialize QP operations and register completion handler */
    rc = pvrdma_qp_ops_init();
    if (rc < 0) {
        rdma_error_report("Failed to initialize QP operations (rc=%d)", rc);
        return -EIO;
    }
    rdma_info_report(
        "QP operations initialized, completion handler registered");

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
        rdma_warn_report("pvrdma_regs_read: handle is NULL, returning 0");
        return 0;
    }

    /* Forward to QEMU register read implementation */
    val = (uint32_t)pvrdma_regs_read_impl(pvrdma, offset, size);

    /* Ensure we never return an error value that could be misinterpreted */
    /* If the implementation returns an error (negative), return 0 instead */
    if ((int32_t)val < 0) {
        rdma_warn_report("pvrdma_regs_read: implementation returned error "
                         "value %d for offset 0x%lx, returning 0",
                         (int32_t)val, offset);
        return 0;
    }

    return val;
}

/*
 * UAR Access (BAR2)
 */

void pvrdma_uar_write(pvrdma_handle_t handle, hwaddr offset, uint32_t value,
                      unsigned size)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;

    rdma_info_report(">>> WRAPPER: pvrdma_uar_write called: handle=%p "
                     "offset=0x%lx val=0x%x size=%u",
                     handle, (unsigned long)offset, value, size);

    if (!pvrdma) {
        rdma_error_report(">>> WRAPPER: pvrdma handle is NULL!");
        return;
    }

    rdma_info_report(">>> WRAPPER: Forwarding to pvrdma_uar_write_impl");
    /* Forward to QEMU UAR write implementation */
    pvrdma_uar_write_impl(pvrdma, offset, value, size);
    rdma_info_report(">>> WRAPPER: pvrdma_uar_write_impl returned");
}

uint32_t pvrdma_uar_read(pvrdma_handle_t handle, hwaddr offset, unsigned size)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;
    uint32_t val = 0;

    if (!pvrdma) {
        return 0;
    }

    val = (uint32_t)pvrdma_uar_read_impl(pvrdma, offset, size);

    return val;
}

void pvrdma_bar0_mmio_count(pvrdma_handle_t handle, bool is_write)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;

    if (!pvrdma) {
        return;
    }
    if (is_write) {
        pvrdma->stats.bar0_writes++;
    } else {
        pvrdma->stats.bar0_reads++;
    }
}

/*
 * Command Execution - pvrdma_exec_cmd is implemented in pvrdma_cmd.c
 */

/*
 * Statistics
 */

void pvrdma_get_stats(pvrdma_handle_t handle, uint64_t *commands,
                      uint64_t *regs_reads, uint64_t *regs_writes,
                      uint64_t *uar_writes, uint64_t *interrupts,
                      uint64_t *uar_reads, uint64_t *bar0_reads,
                      uint64_t *bar0_writes)
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
    if (uar_reads)
        *uar_reads = pvrdma->stats.uar_reads;
    if (bar0_reads)
        *bar0_reads = pvrdma->stats.bar0_reads;
    if (bar0_writes)
        *bar0_writes = pvrdma->stats.bar0_writes;
}

void pvrdma_set_stats_file(pvrdma_handle_t handle, const char *stats_file)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;

    if (!pvrdma) {
        return;
    }

    free(pvrdma->stats.stats_file);
    if (stats_file) {
        pvrdma->stats.stats_file = strdup(stats_file);
    } else {
        pvrdma->stats.stats_file = NULL;
    }
}

void pvrdma_set_stats_instance_info(pvrdma_handle_t handle,
                                    const char *socket_path,
                                    const char *backend_type_str)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;

    if (!pvrdma) {
        return;
    }

    free(pvrdma->stats_socket_path);
    pvrdma->stats_socket_path = socket_path ? strdup(socket_path) : NULL;

    free(pvrdma->stats_backend_str);
    pvrdma->stats_backend_str =
        backend_type_str ? strdup(backend_type_str) : NULL;
}

void pvrdma_set_stats_pci_ids(pvrdma_handle_t handle, uint16_t vid,
                              uint16_t did)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;

    if (pvrdma) {
        pvrdma->stats_pci_vid = vid;
        pvrdma->stats_pci_did = did;
    }
}

void pvrdma_set_stats_connection_state(pvrdma_handle_t handle,
                                       const char *connection_str)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;

    if (!pvrdma) {
        return;
    }

    free(pvrdma->stats_connection_str);
    pvrdma->stats_connection_str =
        connection_str ? strdup(connection_str) : NULL;
}

void pvrdma_inc_stats_flr_count(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;

    if (pvrdma) {
        pvrdma->stats.flr_reset_count++;
    }
}

void pvrdma_write_stats(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;

    if (!pvrdma) {
        return;
    }

    pvrdma_write_stats_impl(pvrdma);
}

void pvrdma_drain_pending_interrupts(pvrdma_handle_t handle)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;

    if (!pvrdma) {
        return;
    }

    pvrdma_drain_deferred_completions();

    if (__atomic_exchange_n(&pvrdma->pending_cq_interrupt, 0,
                            __ATOMIC_ACQUIRE)) {
        post_interrupt(pvrdma, INTR_VEC_CMD_COMPLETION_Q);
    }
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
    rdma_info_report("  guest_addr=%#lx requested_len=%zu dir=%d", addr,
                     plen ? (size_t)*plen : 0, dir);

    /* Check for obviously invalid addresses */
    if (addr == 0) {
        rdma_error_report("DMA map: guest address is NULL!");
        if (plen)
            *plen = 0;
        return NULL;
    }

    if (!dev) {
        rdma_error_report("DMA map: NULL device pointer");
        if (plen)
            *plen = 0;
        return NULL;
    }

    rdma_info_report("  dev=%p", dev);

    if (!dev->vfu_ctx) {
        rdma_error_report("DMA map: NULL vfu_ctx in PCIDevice (dev=%p)", dev);
        rdma_error_report(
            "  This means vfu_ctx was not set in pvrdma_device_create");
        if (plen)
            *plen = 0;
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
        rdma_error_report("DMA map: Failed to allocate SG entry of size %zu",
                          sg_size);
        *plen = 0;
        return NULL;
    }

    rdma_info_report("  Calling vfu_addr_to_sgl(ctx=%p, addr=%#lx, len=%zu, "
                     "sg=%p, cnt=1, prot=RW)",
                     vfu_ctx, addr, (size_t)*plen, sg);

    /* Convert guest physical address to scatter-gather list */
    ret = vfu_addr_to_sgl(vfu_ctx, (vfu_dma_addr_t)(uintptr_t)addr, *plen, sg,
                          1, PROT_READ | PROT_WRITE);
    if (ret < 0) {
        rdma_error_report(
            "DMA map: vfu_addr_to_sgl FAILED for addr=%#lx: %s (errno=%d)",
            addr, strerror(errno), errno);
        rdma_error_report("  This usually means the guest address is not in "
                          "any registered DMA region");
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
        rdma_error_report(
            "DMA map: vfu_sgl_get FAILED for addr=%#lx: %s (errno=%d)", addr,
            strerror(errno), errno);
        rdma_error_report("  This usually means the memory is not mapped "
                          "(vaddr=NULL in DMA region)");
        free(sg);
        *plen = 0;
        return NULL;
    }

    rdma_info_report("  vfu_sgl_get returned %d (success)", ret);

    host_addr = iov.iov_base;
    *plen = iov.iov_len; /* Update with actual mapped length */

    rdma_info_report("  iov.iov_base=%p iov.iov_len=%zu", host_addr,
                     iov.iov_len);

    if (!host_addr) {
        rdma_error_report(
            "DMA map: vfu_sgl_get returned NULL iov_base for guest=%#lx", addr);
        rdma_error_report("  This means the region is not memory-mapped");
        free(sg);
        *plen = 0;
        return NULL;
    }

    rdma_info_report("=== DMA MAP SUCCESS: guest=%#lx -> host=%p len=%zu ===",
                     addr, host_addr, (size_t)*plen);

    /*
     * Store the SGL and iovec for later vfu_sgl_put().
     * This is CRITICAL for memory coherency - writes won't be visible to the
     * guest until we call vfu_sgl_put() on the same SGL/iovec pair.
     */
    if (num_dma_mappings >= MAX_DMA_MAPPINGS) {
        rdma_error_report("DMA map: Mapping table full (%d entries)",
                          MAX_DMA_MAPPINGS);
        /* Still return the pointer, but we won't be able to properly release it
         */
    } else {
        dma_mappings[num_dma_mappings].guest_addr = addr;
        dma_mappings[num_dma_mappings].len = *plen;
        dma_mappings[num_dma_mappings].sg = sg;
        dma_mappings[num_dma_mappings].iov = iov;
        dma_mappings[num_dma_mappings].host_addr = host_addr;
        dma_mappings[num_dma_mappings].vfu_ctx = vfu_ctx;
        num_dma_mappings++;
        rdma_info_report("DMA map: Stored mapping #%d (guest=%#lx)",
                         num_dma_mappings, addr);
    }

    return host_addr;
}

/*
 * Helper: Flush DSR writes by doing put/get cycle
 * This ensures cache coherency and notifies libvfio-user/QEMU of changes.
 */
void pvrdma_dsr_flush(void *handle)
{
    PVRDMADev *pvrdma = (PVRDMADev *)handle;
    dma_addr_t dsr_guest_addr = pvrdma->dsr_info.dma;

    rdma_info_report(">>> pvrdma_dsr_flush: START - Flushing DSR at guest=%#lx",
                     dsr_guest_addr);

    /* Find the DSR mapping */
    for (int i = 0; i < num_dma_mappings; i++) {
        dma_mapping_t *mapping = &dma_mappings[i];

        if (mapping->guest_addr == dsr_guest_addr) {
            rdma_info_report("  Found DSR mapping #%d at host=%p", i,
                             mapping->host_addr);

            /* Verify current values BEFORE flush */
            struct pvrdma_device_shared_region *dsr =
                (struct pvrdma_device_shared_region *)mapping->host_addr;
            rdma_info_report("  BEFORE flush: mode=%d gid_types=0x%x",
                             dsr->caps.mode, dsr->caps.gid_types);

            /* Per libvfio-user samples/server.c pattern:
             * Call vfu_sgl_put() to release and mark dirty.
             * DO NOT immediately re-acquire - only get when needed for next
             * access.
             *
             * From server.c:
             *   vfu_sgl_get(vfu_ctx, sg, &iov, 1, 0);
             *   memcpy(iov.iov_base, &buf[i * size], size);
             *   vfu_sgl_put(vfu_ctx, sg, &iov, 1);  // <-- Release immediately!
             */
            rdma_info_report(
                "  Calling vfu_sgl_put() to flush and RELEASE mapping...");
            if (mapping->vfu_ctx && mapping->sg) {
                vfu_sgl_put(mapping->vfu_ctx, mapping->sg, &mapping->iov, 1);
            }

            /* Mark mapping as released */
            mapping->host_addr = NULL;
            mapping->iov.iov_base = NULL;
            mapping->iov.iov_len = 0;

            rdma_info_report(
                "  vfu_sgl_put() complete - DSR released and marked dirty");
            rdma_info_report(
                "<<< pvrdma_dsr_flush: COMPLETE - DSR mapping RELEASED");
            return;
        }
    }

    rdma_error_report("pvrdma_dsr_flush: ERROR - DSR mapping not found!");
}

/*
 * Helper: Sync DMA writes back to guest by calling vfu_sgl_put()
 * This releases the memory mapping and ensures writes are visible to the guest.
 */
int pci_dma_sync(PCIDevice *dev, dma_addr_t guest_addr, dma_addr_t len)
{
    (void)dev;

    rdma_info_report(
        "=== DMA SYNC: Searching for mapping at guest=%#lx len=%zu ===",
        guest_addr, (size_t)len);

    /* Find the mapping that contains this address */
    for (int i = 0; i < num_dma_mappings; i++) {
        dma_mapping_t *mapping = &dma_mappings[i];

        /* Check if this address is within the mapped region */
        if (guest_addr >= mapping->guest_addr &&
            guest_addr + len <= mapping->guest_addr + mapping->len) {
            rdma_info_report(
                "DMA sync: Found mapping #%d: guest=%#lx len=%zu sg=%p", i,
                mapping->guest_addr, mapping->len, mapping->sg);

            /* Call vfu_sgl_put() to sync writes back to guest */
            if (!mapping->vfu_ctx || !mapping->sg) {
                rdma_error_report("DMA sync: NULL vfu_ctx or sg "
                                  "for mapping #%d",
                                  i);
                return -EINVAL;
            }
            vfu_sgl_put(mapping->vfu_ctx, mapping->sg, &mapping->iov, 1);

            rdma_info_report("DMA sync: vfu_sgl_put() called - "
                             "writes should now be visible");

            /* Re-acquire the mapping for future use */
            int ret =
                vfu_sgl_get(mapping->vfu_ctx, mapping->sg, &mapping->iov, 1, 0);
            if (ret < 0) {
                rdma_error_report("DMA sync: Failed to re-acquire mapping: %s",
                                  strerror(errno));
                return -1;
            }

            rdma_info_report("DMA sync: Mapping re-acquired successfully");

            /* Verify the write by reading back from the iovec */
            if (len >= 4 && mapping->iov.iov_base) {
                uint32_t *data =
                    (uint32_t *)((char *)mapping->iov.iov_base +
                                 (guest_addr - mapping->guest_addr));
                rdma_info_report("DMA sync: VERIFICATION - First 4 bytes at "
                                 "offset 0: 0x%08x",
                                 data[0]);
                rdma_info_report("DMA sync: VERIFICATION - First 4 bytes at "
                                 "offset 4: 0x%08x",
                                 data[1]);
            }

            return 0;
        }
    }

    rdma_error_report("DMA sync: No mapping found for guest=%#lx", guest_addr);
    return -1;
}

/* Implement pci_dma_unmap - called by QEMU PVRDMA code via hw/pci/pci.h */
void pci_dma_unmap(PCIDevice *dev, void *buffer, dma_addr_t len, int dir,
                   dma_addr_t access_len)
{
    (void)dev;
    (void)len;
    (void)dir;
    (void)access_len;

    rdma_info_report("=== DMA UNMAP: buffer=%p ===", buffer);

    /* Find and release the mapping */
    for (int i = 0; i < num_dma_mappings; i++) {
        if (dma_mappings[i].host_addr == buffer) {
            rdma_info_report("DMA unmap: Found mapping #%d (guest=%#lx)", i,
                             dma_mappings[i].guest_addr);

            /* Release the SGL mapping */
            if (dma_mappings[i].vfu_ctx && dma_mappings[i].sg) {
                vfu_sgl_put(dma_mappings[i].vfu_ctx, dma_mappings[i].sg,
                            &dma_mappings[i].iov, 1);
            }

            /* Free the SG structure */
            free(dma_mappings[i].sg);
            dma_mappings[i].sg = NULL; /* Prevent double-free */

            /* Remove from table by shifting remaining entries */
            for (int j = i; j < num_dma_mappings - 1; j++) {
                dma_mappings[j] = dma_mappings[j + 1];
            }
            num_dma_mappings--;

            /* Clear the last entry (now unused) */
            memset(&dma_mappings[num_dma_mappings], 0, sizeof(dma_mappings[0]));

            rdma_info_report(
                "DMA unmap: Released and removed mapping (now %d mappings)",
                num_dma_mappings);
            return;
        }
    }

    rdma_debug_report(
        "DMA unmap: Mapping not found for buffer=%p (already unmapped?)",
        buffer);
}

/*
 * Interrupt Handling (called FROM QEMU code)
 */

void post_interrupt(PVRDMADev *pvrdma, unsigned vector)
{
    vfu_ctx_t *vfu_ctx;
    int ret;

    if (!pvrdma || !pvrdma->parent_obj.vfu_dev || !pvrdma->parent_obj.vfu_ctx) {
        rdma_error_report("post_interrupt: invalid device pointer");
        return;
    }

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
        rdma_error_report("Failed to trigger IRQ vector %u: %s", vector,
                          strerror(errno));
        return;
    }

    pvrdma->stats.interrupts++;
}
