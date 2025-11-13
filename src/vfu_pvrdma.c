/*
 * libvfio-user PVRDMA Device Server
 *
 * Implements a userspace PVRDMA (ParaVirtualized RDMA) device using
 * libvfio-user. This server emulates a VMware PVRDMA PCIe device that can be
 * attached to a VM.
 *
 * This version integrates the QEMU PVRDMA device implementation through a
 * compatibility bridge layer.
 *
 * Copyright (C) 2025
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syslog.h>
#include <getopt.h>
#include <assert.h>

#include <vfio-user/libvfio-user.h>
#include <linux/pci_regs.h>

/* Internal headers - NO QEMU headers! */
#include "vfu_pvrdma_internal.h"
#include "vfu_compat_bridge.h"

/* PCI Device IDs for VMware PVRDMA */
/* AMD Emulated RDMA device IDs (for vfio-user) */
#define PCI_VENDOR_ID_AMD 0x1022
#define PCI_DEVICE_ID_AMD_EMRDMA 0x1484

/* PCI Class Codes (from linux/pci_ids.h) */
#define PCI_BASE_CLASS_NETWORK 0x02

/* Socket path for vfio-user communication */
#define DEFAULT_SOCKET_PATH "/tmp/vfio-user-pvrdma.sock"

/* Global context for signal handling */
static vfu_ctx_t *g_vfu_ctx = NULL;
static volatile sig_atomic_t g_shutdown_requested = 0;

/**
 * Signal handler for graceful shutdown
 */
static void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        g_shutdown_requested = 1;
    }
}

/**
 * Log callback for libvfio-user
 */
static void vfu_log_cb(vfu_ctx_t *vfu_ctx, int level, const char *msg)
{
    const char *prefix = "vfu_pvrdma";

    switch (level) {
    case LOG_EMERG:
    case LOG_ALERT:
    case LOG_CRIT:
    case LOG_ERR:
        fprintf(stderr, "%s: ERROR: %s\n", prefix, msg);
        break;
    case LOG_WARNING:
        fprintf(stderr, "%s: WARN: %s\n", prefix, msg);
        break;
    case LOG_NOTICE:
    case LOG_INFO:
        printf("%s: %s\n", prefix, msg);
        break;
    case LOG_DEBUG:
        printf("%s: DEBUG: %s\n", prefix, msg);
        break;
    }
}

/**
 * BAR0 (MSI-X) access callback
 */
static ssize_t bar0_access(vfu_ctx_t *vfu_ctx, char *buf, size_t count,
                           loff_t offset, bool is_write)
{
    vfu_pvrdma_dev_t *dev = vfu_get_private(vfu_ctx);

    if (offset + count > RDMA_BAR0_MSIX_SIZE) {
        vfu_log(vfu_ctx, LOG_ERR,
                "BAR0 access out of bounds: offset=%#lx count=%zu", offset,
                count);
        errno = EINVAL;
        return -1;
    }

    /* MSI-X table and PBA are handled by libvfio-user */
    /* Any other accesses to BAR0 are just memory reads/writes */

    if (is_write) {
        memcpy(dev->bar0_mem + offset, buf, count);
    } else {
        memcpy(buf, dev->bar0_mem + offset, count);
    }

    return count;
}

/**
 * BAR1 (Registers) access callback
 * Forwards to QEMU PVRDMA register handlers via wrapper API
 */
static ssize_t bar1_access(vfu_ctx_t *vfu_ctx, char *buf, size_t count,
                           loff_t offset, bool is_write)
{
    vfu_pvrdma_dev_t *dev = vfu_get_private(vfu_ctx);
    uint32_t val;

    /* Defensive checks */
    if (!dev) {
        vfu_log(vfu_ctx, LOG_ERR, "BAR1 access: dev is NULL!");
        errno = EFAULT;
        return -1;
    }
    if (!buf) {
        vfu_log(vfu_ctx, LOG_ERR, "BAR1 access: buf is NULL!");
        errno = EFAULT;
        return -1;
    }
    if (!dev->pvrdma_handle) {
        vfu_log(vfu_ctx, LOG_ERR, "BAR1 access: pvrdma_handle is NULL!");
        errno = EFAULT;
        return -1;
    }

    if (offset + count > RDMA_BAR1_REGS_SIZE * sizeof(uint32_t)) {
        vfu_log(vfu_ctx, LOG_ERR,
                "BAR1 access out of bounds: offset=%#lx count=%zu", offset,
                count);
        errno = EINVAL;
        return -1;
    }

    /* Register accesses must be 32-bit aligned */
    if (count != sizeof(uint32_t) || (offset & 0x3)) {
        vfu_log(vfu_ctx, LOG_ERR,
                "BAR1 access not 32-bit aligned: offset=%#lx count=%zu", offset,
                count);
        errno = EINVAL;
        return -1;
    }

    if (is_write) {
        /* Forward write to QEMU register handler via wrapper */
        memcpy(&val, buf, sizeof(val));
        pvrdma_regs_write(dev->pvrdma_handle, offset, val, sizeof(val));

        vfu_log(vfu_ctx, LOG_DEBUG, "BAR1 write: offset=%#lx val=%#x", offset,
                val);
    } else {
        /* Forward read to QEMU register handler via wrapper */
        val = pvrdma_regs_read(dev->pvrdma_handle, offset, sizeof(val));
        memcpy(buf, &val, sizeof(val));

        vfu_log(vfu_ctx, LOG_DEBUG, "BAR1 read: offset=%#lx val=%#x", offset,
                val);
    }

    return count;
}

/**
 * BAR2 (UAR - User Access Region) access callback
 * Forwards to QEMU PVRDMA UAR handlers via wrapper API
 */
static ssize_t bar2_access(vfu_ctx_t *vfu_ctx, char *buf, size_t count,
                           loff_t offset, bool is_write)
{
    vfu_pvrdma_dev_t *dev = vfu_get_private(vfu_ctx);
    uint32_t val;

    if (offset + count > RDMA_BAR2_UAR_SIZE * sizeof(uint32_t)) {
        vfu_log(vfu_ctx, LOG_ERR,
                "BAR2 access out of bounds: offset=%#lx count=%zu", offset,
                count);
        errno = EINVAL;
        return -1;
    }

    if (is_write) {
        /* UAR writes are typically doorbells */
        memcpy(&val, buf, (count < sizeof(val)) ? count : sizeof(val));

        /* Forward to QEMU UAR handler via wrapper */
        pvrdma_uar_write(dev->pvrdma_handle, offset, val, sizeof(val));

        vfu_log(vfu_ctx, LOG_DEBUG, "BAR2 (UAR) write: offset=%#lx val=%#x",
                offset, val);
    } else {
        /* UAR reads */
        val = pvrdma_uar_read(dev->pvrdma_handle, offset, sizeof(val));
        memcpy(buf, &val, (count < sizeof(val)) ? count : sizeof(val));

        vfu_log(vfu_ctx, LOG_DEBUG, "BAR2 (UAR) read: offset=%#lx val=%#x",
                offset, val);
    }

    return count;
}

/**
 * Device reset callback
 */
static int device_reset_cb(vfu_ctx_t *vfu_ctx, vfu_reset_type_t type)
{
    vfu_pvrdma_dev_t *dev = vfu_get_private(vfu_ctx);

    vfu_log(vfu_ctx, LOG_INFO, "Device reset requested (type=%d)", type);

    switch (type) {
    case VFU_RESET_DEVICE:
        /* Reset device state but keep context alive */
        dev->device_active = false;
        break;

    case VFU_RESET_LOST_CONN:
        /* Client disconnected, prepare for new connection */
        vfu_log(vfu_ctx, LOG_INFO, "Client connection lost");
        dev->device_active = false;
        break;

    case VFU_RESET_PCI_FLR:
        /* PCI Function Level Reset */
        vfu_log(vfu_ctx, LOG_INFO, "PCI FLR requested");
        dev->device_active = false;
        break;
    }

    return 0;
}

/**
 * DMA region registration callback
 */
static void dma_register_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
    vfu_log(vfu_ctx, LOG_DEBUG,
            "DMA region registered: iova=%p len=%zu vaddr=%p prot=%#x",
            info->iova.iov_base, info->iova.iov_len, info->vaddr, info->prot);

    /* DMA regions are now available for mapping guest memory */
}

/**
 * DMA region unregistration callback
 */
static void dma_unregister_cb(vfu_ctx_t *vfu_ctx, vfu_dma_info_t *info)
{
    vfu_log(vfu_ctx, LOG_DEBUG, "DMA region unregistered: iova=%p len=%zu",
            info->iova.iov_base, info->iova.iov_len);
}

/**
 * Initialize PVRDMA device structure via wrapper API
 */
static int pvrdma_device_init(vfu_pvrdma_dev_t *dev)
{
    int ret;
    
    /* Create PVRDMA device using wrapper API with selected backend */
    dev->pvrdma_handle =
        pvrdma_device_create(dev, dev->backend_type_str,
                             dev->backend_device_name,
                             dev->backend_eth_device, dev->backend_port_num);

    if (!dev->pvrdma_handle) {
        fprintf(stderr, "Failed to create PVRDMA device\n");
        return -1;
    }
    
    /* Realize the device - this initializes registers and backends */
    ret = pvrdma_device_realize(dev->pvrdma_handle);
    if (ret < 0) {
        fprintf(stderr, "Failed to realize PVRDMA device with backend '%s'\n",
                dev->backend_type_str);
        return -1;
    }

    dev->device_initialized = true;

    printf("PVRDMA device initialized successfully with '%s' backend\n",
           dev->backend_type_str);

    return 0;
}

/**
 * Setup PCI configuration for PVRDMA device
 */
static int setup_pci_config(vfu_ctx_t *vfu_ctx, vfu_pvrdma_dev_t *dev)
{
    int ret;

    /* Initialize PCI device */
    ret =
        vfu_pci_init(vfu_ctx, VFU_PCI_TYPE_EXPRESS, PCI_HEADER_TYPE_NORMAL, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "vfu_pci_init() failed");
    }

    /* Set vendor/device IDs */
    vfu_pci_set_id(vfu_ctx, PCI_VENDOR_ID_AMD,       /* Vendor ID */
                   PCI_DEVICE_ID_AMD_EMRDMA,      /* Device ID */
                   PCI_VENDOR_ID_AMD,             /* Subsystem Vendor ID */
                   PCI_DEVICE_ID_AMD_EMRDMA);     /* Subsystem ID */

    /* Set PCI class code: Network Controller - Other */
    vfu_pci_set_class(vfu_ctx, PCI_BASE_CLASS_NETWORK, /* Base class */
                      0x80,                            /* Sub class (other) */
                      0x00); /* Programming interface */

    vfu_log(vfu_ctx, LOG_INFO, "PCI device configured: vendor=%#x device=%#x",
            PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_EMRDMA);

    return 0;
}

/**
 * Setup BARs (Base Address Registers)
 */
static int setup_bars(vfu_ctx_t *vfu_ctx, vfu_pvrdma_dev_t *dev)
{
    int ret;

    /* Allocate BAR memory */
    dev->bar0_mem = calloc(1, RDMA_BAR0_MSIX_SIZE);
    dev->bar1_mem = calloc(1, RDMA_BAR1_REGS_SIZE * sizeof(uint32_t));
    dev->bar2_mem = calloc(1, RDMA_BAR2_UAR_SIZE * sizeof(uint32_t));

    if (!dev->bar0_mem || !dev->bar1_mem || !dev->bar2_mem) {
        err(EXIT_FAILURE, "Failed to allocate BAR memory");
    }

    /* Setup BAR0: MSI-X (16KB, memory-mapped) */
    ret = vfu_setup_region(
        vfu_ctx, VFU_PCI_DEV_BAR0_REGION_IDX, RDMA_BAR0_MSIX_SIZE, bar0_access,
        VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, NULL, 0, -1, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "Failed to setup BAR0");
    }

    /* Setup BAR1: Registers (256 bytes, memory-mapped) */
    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR1_REGION_IDX,
                           RDMA_BAR1_REGS_SIZE * sizeof(uint32_t), bar1_access,
                           VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, NULL, 0,
                           -1, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "Failed to setup BAR1");
    }

    /* Setup BAR2: UAR - User Access Region (variable size, memory-mapped) */
    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR2_REGION_IDX,
                           RDMA_BAR2_UAR_SIZE * sizeof(uint32_t), bar2_access,
                           VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, NULL, 0,
                           -1, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "Failed to setup BAR2");
    }

    vfu_log(vfu_ctx, LOG_INFO, "BARs configured: BAR0=%zu BAR1=%zu BAR2=%zu",
            (size_t)RDMA_BAR0_MSIX_SIZE,
            (size_t)(RDMA_BAR1_REGS_SIZE * sizeof(uint32_t)),
            (size_t)(RDMA_BAR2_UAR_SIZE * sizeof(uint32_t)));

    return 0;
}

/**
 * Setup MSI-X interrupts
 *
 * MSI-X setup requires:
 * 1. Add MSI-X capability to PCI config space
 * 2. Setup interrupt vectors with vfu_setup_device_nr_irqs()
 * 3. libvfio-user will then manage the table/PBA in BAR0
 */
static int setup_interrupts(vfu_ctx_t *vfu_ctx, vfu_pvrdma_dev_t *dev)
{
    ssize_t ret;
    
    /* MSI-X Table and PBA offsets within BAR0
     * Table: starts at offset 0x0, size = vectors * 16 bytes
     * PBA: starts at offset 0x2000 (8KB)
     */
    #define MSIX_TABLE_OFFSET 0x0000
    #define MSIX_PBA_OFFSET   0x2000
    #define MSIX_TABLE_BIR    0  /* Table in BAR 0 */
    #define MSIX_PBA_BIR      0  /* PBA in BAR 0 */

    /* Setup legacy INTx interrupt (required by some guests for PCI compliance) */
    ret = vfu_setup_device_nr_irqs(vfu_ctx, VFU_DEV_INTX_IRQ, 1);
    if (ret < 0) {
        vfu_log(vfu_ctx, LOG_ERR, "Failed to setup INTx interrupt: %m");
        return ret;
    }

    /* MSI-X capability structure (12 bytes total) */
    struct {
        uint8_t id;         /* Capability ID = 0x11 for MSI-X */
        uint8_t next;       /* Next capability pointer (0 = none, filled by lib) */
        uint16_t ctrl;      /* Message Control register */
        uint32_t table;     /* Table Offset/BIR */
        uint32_t pba;       /* PBA Offset/BIR */
    } __attribute__((packed)) msix_cap;
    
    /* Build MSI-X capability structure */
    msix_cap.id = PCI_CAP_ID_MSIX;  /* 0x11 */
    msix_cap.next = 0;  /* Will be filled by libvfio-user if there are more caps */
    
    /* Message Control: bits [10:0] = Table Size-1 (so 2 for 3 vectors) 
     * bit [14] = Function Mask (0 = not masked)
     * bit [15] = MSI-X Enable (will be set by guest driver) 
     */
    msix_cap.ctrl = (RDMA_MAX_INTRS - 1) & 0x7FF;  /* Table size = 3-1 = 2 */
    
    /* Table Offset/BIR: bits [2:0] = BIR, bits [31:3] = offset >> 3 */
    msix_cap.table = (MSIX_TABLE_OFFSET & 0xFFFFFFF8) | (MSIX_TABLE_BIR & 0x7);
    
    /* PBA Offset/BIR: bits [2:0] = BIR, bits [31:3] = offset >> 3 */
    msix_cap.pba = (MSIX_PBA_OFFSET & 0xFFFFFFF8) | (MSIX_PBA_BIR & 0x7);

    /* Add MSI-X capability to PCI config space at automatic position (pos=0) */
    ret = vfu_pci_add_capability(vfu_ctx, 0, 0, &msix_cap);
    if (ret < 0) {
        vfu_log(vfu_ctx, LOG_ERR, "Failed to add MSI-X capability: %m");
        return ret;
    }
    
    vfu_log(vfu_ctx, LOG_INFO, "Added MSI-X capability at offset 0x%zx", ret);

    /* Setup interrupt vector count - libvfio-user will manage table/PBA */
    ret = vfu_setup_device_nr_irqs(vfu_ctx, VFU_DEV_MSIX_IRQ, RDMA_MAX_INTRS);
    if (ret < 0) {
        vfu_log(vfu_ctx, LOG_ERR, "Failed to setup MSI-X IRQ count: %m");
        return ret;
    }

    vfu_log(vfu_ctx, LOG_INFO, 
            "Interrupts configured: INTx=1, MSI-X=%d vectors (table=BAR%d:0x%x, pba=BAR%d:0x%x)",
            RDMA_MAX_INTRS, MSIX_TABLE_BIR, MSIX_TABLE_OFFSET, MSIX_PBA_BIR, MSIX_PBA_OFFSET);

    return 0;
}

/**
 * Print usage information
 */
static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s [OPTIONS]\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -s, --socket PATH    Socket path (default: %s)\n",
            DEFAULT_SOCKET_PATH);
    fprintf(stderr, "  -b, --backend TYPE   RDMA backend: none|loopback|verbs[:device]\n");
    fprintf(stderr, "                       (default: none)\n");
    fprintf(stderr, "  -d, --device NAME    InfiniBand device name (for verbs backend)\n");
    fprintf(stderr, "  -e, --ethdev NAME    Ethernet device name\n");
    fprintf(stderr, "  -p, --port NUM       IB port number (default: 1)\n");
    fprintf(stderr, "  -v, --verbose        Enable verbose logging\n");
    fprintf(stderr, "  -h, --help           Show this help message\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Backend Types:\n");
    fprintf(stderr, "  none         - No RDMA backend (minimal stubs)\n");
    fprintf(stderr, "  loopback[:opts] - Internal loopback emulation (for testing)\n");
    fprintf(stderr, "               Options (comma-separated):\n");
    fprintf(stderr, "                 preserve  - Use actual guest data (default)\n");
    fprintf(stderr, "                 zeros     - Fill with 0x00\n");
    fprintf(stderr, "                 ones      - Fill with 0xFF\n");
    fprintf(stderr, "                 increment - Fill with 0x00,0x01,0x02,...\n");
    fprintf(stderr, "                 decrement - Fill with 0xFF,0xFE,0xFD,...\n");
    fprintf(stderr, "                 alternate - Fill with 0xAA,0x55,0xAA,...\n");
    fprintf(stderr, "                 random    - Fill with random data\n");
    fprintf(stderr, "                 md5       - Compute MD5 hash of data\n");
    fprintf(stderr, "               Examples:\n");
    fprintf(stderr, "                 loopback           - Use guest data, no MD5\n");
    fprintf(stderr, "                 loopback:md5       - Use guest data, compute MD5\n");
    fprintf(stderr, "                 loopback:random,md5 - Random data with MD5\n");
    fprintf(stderr, "                 loopback:zeros     - All zeros, no MD5\n");
    fprintf(stderr, "  verbs[:dev]  - libibverbs hardware backend\n");
}

/**
 * Main entry point
 */
int main(int argc, char *argv[])
{
    vfu_ctx_t *vfu_ctx;
    vfu_pvrdma_dev_t *dev;
    const char *socket_path = DEFAULT_SOCKET_PATH;
    struct sigaction sa;
    int ret, opt;

    static struct option long_options[] = {
        {"socket", required_argument, 0, 's'},
        {"backend", required_argument, 0, 'b'},
        {"device", required_argument, 0, 'd'},
        {"ethdev", required_argument, 0, 'e'},
        {"port", required_argument, 0, 'p'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    /* Allocate device structure */
    dev = calloc(1, sizeof(*dev));
    if (!dev) {
        err(EXIT_FAILURE, "Failed to allocate device structure");
    }

    /* Set defaults */
    dev->backend_type_str = strdup("none");  /* Default to "none" backend */
    dev->backend_port_num = 1;
    dev->verbose = false;
    dev->device_initialized = false;
    dev->device_active = false;

    /* Parse command line options */
    while ((opt = getopt_long(argc, argv, "s:b:d:e:p:vh", long_options, NULL)) !=
           -1) {
        switch (opt) {
        case 's':
            socket_path = optarg;
            break;
        case 'b':
            free(dev->backend_type_str);
            dev->backend_type_str = strdup(optarg);
            break;
        case 'd':
            dev->backend_device_name = strdup(optarg);
            break;
        case 'e':
            dev->backend_eth_device = strdup(optarg);
            break;
        case 'p':
            dev->backend_port_num = atoi(optarg);
            break;
        case 'v':
            dev->verbose = true;
            break;
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    /* Setup signal handlers */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        err(EXIT_FAILURE, "Failed to setup signal handlers");
    }

    printf("vfu_pvrdma: Starting PVRDMA device server (Multi-Backend Support)\n");
    printf("  Socket: %s\n", socket_path);
    printf("  Backend: %s\n", dev->backend_type_str);
    if (dev->backend_device_name) {
        printf("  IB Device: %s\n", dev->backend_device_name);
    }
    if (dev->backend_eth_device) {
        printf("  Eth Device: %s\n", dev->backend_eth_device);
    }
    printf("  IB Port: %u\n", dev->backend_port_num);
    printf("\n");
    printf("Features in this build:\n");
    printf("  ✓ PCI device enumeration\n");
    printf("  ✓ BAR0/1/2 access\n");
    printf("  ✓ DSR register handling (QEMU integration)\n");
    printf("  ✓ Command channel framework\n");
    printf("  ✓ Multi-backend support (none/loopback/verbs)\n");
    printf("  ⚠ Full command processing (in progress)\n");
    printf("\n");

    /* Remove old socket if it exists - try multiple approaches */
    struct stat st;
    if (stat(socket_path, &st) == 0) {
        if (S_ISSOCK(st.st_mode)) {
            printf("Removing stale socket file: %s\n", socket_path);
            if (unlink(socket_path) != 0) {
                warn("Failed to unlink existing socket");
            }
        }
    }
    
    /* Give the system a moment to release the socket */
    usleep(100000); /* 100ms */

    /* Create libvfio-user context with non-blocking attach */
    vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, socket_path, 
                            LIBVFIO_USER_FLAG_ATTACH_NB, dev, VFU_DEV_TYPE_PCI);
    if (!vfu_ctx) {
        err(EXIT_FAILURE, "vfu_create_ctx() failed");
    }
    g_vfu_ctx = vfu_ctx;
    dev->vfu_ctx = vfu_ctx;

    /* Setup logging */
    ret =
        vfu_setup_log(vfu_ctx, vfu_log_cb, dev->verbose ? LOG_DEBUG : LOG_INFO);
    if (ret < 0) {
        err(EXIT_FAILURE, "vfu_setup_log() failed");
    }

    /* Initialize PVRDMA device */
    if (pvrdma_device_init(dev) < 0) {
        err(EXIT_FAILURE, "pvrdma_device_init() failed");
    }

    /* Setup PCI configuration */
    if (setup_pci_config(vfu_ctx, dev) < 0) {
        err(EXIT_FAILURE, "setup_pci_config() failed");
    }

    /* Setup BARs */
    if (setup_bars(vfu_ctx, dev) < 0) {
        err(EXIT_FAILURE, "setup_bars() failed");
    }

    /* Setup interrupts */
    if (setup_interrupts(vfu_ctx, dev) < 0) {
        err(EXIT_FAILURE, "setup_interrupts() failed");
    }

    /* Setup DMA callbacks */
    ret = vfu_setup_device_dma(vfu_ctx, dma_register_cb, dma_unregister_cb);
    if (ret < 0) {
        err(EXIT_FAILURE, "vfu_setup_device_dma() failed");
    }

    /* Setup reset callback */
    ret = vfu_setup_device_reset_cb(vfu_ctx, device_reset_cb);
    if (ret < 0) {
        err(EXIT_FAILURE, "vfu_setup_device_reset_cb() failed");
    }

    /* Realize the device */
    ret = vfu_realize_ctx(vfu_ctx);
    if (ret < 0) {
        err(EXIT_FAILURE, "vfu_realize_ctx() failed");
    }

    /* Set socket permissions to allow non-root QEMU to connect */
    if (chmod(socket_path, 0666) < 0) {
        fprintf(stderr, "vfu_pvrdma: WARNING: Failed to set socket permissions: %s\n",
                strerror(errno));
        fprintf(stderr, "vfu_pvrdma: You may need to manually run: sudo chmod 666 %s\n",
                socket_path);
    } else {
        printf("vfu_pvrdma: ✓ Socket permissions set to 0666 (rw-rw-rw-) for %s\n", 
               socket_path);
        fflush(stdout);
    }

    vfu_log(vfu_ctx, LOG_INFO,
            "Device realized, waiting for client connection...");

    /* Main loop */
    while (!g_shutdown_requested) {
        /* Attach to client (non-blocking) */
        ret = vfu_attach_ctx(vfu_ctx);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* No client yet, sleep and retry */
                usleep(100000); /* 100ms */
                continue;
            } else if (errno == EINTR) {
                /* Interrupted by signal, check shutdown flag */
                continue;
            }
            vfu_log(vfu_ctx, LOG_ERR, "vfu_attach_ctx() failed with errno=%d: %s", 
                    errno, strerror(errno));
            err(EXIT_FAILURE, "vfu_attach_ctx() failed");
        }

        vfu_log(vfu_ctx, LOG_INFO, "Client connected!");

        /* Run device - process requests from client */
        int loop_count = 0;
        while (!g_shutdown_requested) {
            /* Debug logging disabled - too verbose */
            ret = vfu_run_ctx(vfu_ctx);
            loop_count++;
            if (ret < 0) {
                if (errno == ENOTCONN) {
                    vfu_log(vfu_ctx, LOG_INFO, "Client disconnected after %d loops", loop_count);
                    break;
                } else if (errno == EINTR) {
                    /* Interrupted by signal */
                    vfu_log(vfu_ctx, LOG_INFO, "Interrupted by signal after %d loops", loop_count);
                    break;
                } else {
                    vfu_log(vfu_ctx, LOG_ERR, "vfu_run_ctx() failed after %d loops: %s",
                            loop_count, strerror(errno));
                    break;
                }
            }
        }
        vfu_log(vfu_ctx, LOG_INFO, ">>> Event loop exited after %d iterations", loop_count);
    }

    vfu_log(vfu_ctx, LOG_INFO, "Shutting down");

    /* Cleanup */
    vfu_destroy_ctx(vfu_ctx);

    /* Destroy PVRDMA device */
    if (dev->pvrdma_handle) {
        pvrdma_device_destroy(dev->pvrdma_handle);
    }

    free(dev->bar0_mem);
    free(dev->bar1_mem);
    free(dev->bar2_mem);
    free(dev->backend_device_name);
    free(dev->backend_eth_device);
    free(dev);

    unlink(socket_path);

    printf("vfu_pvrdma: Shutdown complete\n");

    return EXIT_SUCCESS;
}
