/*
 * ROCm ERNIC (Emulated RDMA NIC) Device Server
 *
 * Implements a userspace RDMA device using libvfio-user.
 * This server emulates an AMD RDMA PCIe device that can be attached to a VM.
 *
 * This version integrates RDMA device logic through a compatibility
 * bridge layer.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
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
#include <time.h>

#include <vfio-user/libvfio-user.h>
#include <vfio-user/pci_defs.h>
#include <linux/pci_regs.h>
#include <glib.h> /* For g_main_context_iteration() */

/* Internal headers */
#include "rocm_ernic_internal.h"
#include "rocm_ernic_compat.h"
#include "qemu/error-report.h"
#include "ionic_adminq.h"
#include "ionic_datapath.h"
#include "nvmeof_target.h"
#include "s3_target.h"

static const char *get_backend_type_base(const char *backend_str);

/* PCI identity presented over vfio-user.
 *
 * We emulate an ionic-protocol NIC, so we claim the Pensando Systems
 * vendor ID.  The device ID sits outside the range upstream ionic.ko
 * probes (0x1002 ETH_PF, 0x1003 ETH_VF), which keeps the emulated NIC
 * distinguishable from real DSC hardware in lspci, udev and pci.ids
 * at the cost of the one-line ID patch in
 * patches/0001-ionic-add-AMD-emulated-ionic-device-id.patch. */
#define PCI_VENDOR_ID_PENSANDO        0x1dd8
#define PCI_DEVICE_ID_AMD_IONIC_ERNIC 0x100a

/* PCI Class Codes (from linux/pci_ids.h) */
#define PCI_BASE_CLASS_NETWORK 0x02

/* Socket path for vfio-user communication */
#define DEFAULT_SOCKET_PATH "/tmp/vfio-user-rocm-ernic.sock"

/* Global context for signal handling */
static vfu_ctx_t *g_vfu_ctx = NULL;
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_cleanup_in_progress = 0;

/**
 * Signal handler for graceful shutdown
 */
static void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        if (!g_cleanup_in_progress) {
            g_shutdown_requested = 1;
            /* If we have a vfu_ctx, try to interrupt it */
            if (g_vfu_ctx) {
                /* Signal will cause vfu_run_ctx() to return with EINTR */
            }
        }
    }
}

/**
 * Log callback for libvfio-user
 */
static void vfu_log_cb(vfu_ctx_t *vfu_ctx, int level, const char *msg)
{
    const char *prefix = "rocm-ernic";

    switch (level) {
    case LOG_EMERG:
    case LOG_ALERT:
    case LOG_CRIT:
    case LOG_ERR:
        if (ernic_log_enabled(ERNIC_LOG_ERROR)) {
            fprintf(stderr, "%s: ERROR: %s\n", prefix, msg);
        }
        break;
    case LOG_WARNING:
        if (ernic_log_enabled(ERNIC_LOG_WARN)) {
            fprintf(stderr, "%s: WARN: %s\n", prefix, msg);
        }
        break;
    case LOG_NOTICE:
    case LOG_INFO:
        if (ernic_log_enabled(ERNIC_LOG_INFO)) {
            printf("%s: %s\n", prefix, msg);
        }
        break;
    case LOG_DEBUG:
        if (ernic_log_enabled(ERNIC_LOG_DEBUG)) {
            printf("%s: DEBUG: %s\n", prefix, msg);
        }
        break;
    default:
        break;
    }
}

/**
 * Map the runtime log level onto the libvfio-user syslog threshold, so
 * the library skips formatting messages we would drop anyway.
 */
static int vfu_log_threshold(ErnicLogLevel lvl)
{
    switch (lvl) {
    case ERNIC_LOG_DEBUG:
        return LOG_DEBUG;
    case ERNIC_LOG_INFO:
        return LOG_INFO;
    case ERNIC_LOG_WARN:
        return LOG_WARNING;
    case ERNIC_LOG_ERROR:
    case ERNIC_LOG_NONE:
    default:
        return LOG_ERR;
    }
}

/**
 * BAR0 access callback.
 *
 * Below IONIC_BAR0_REGS_SIZE is the ionic register window, forwarded to
 * ionic_eth_emu.  At and above it is the MSI-X table/PBA, a plain shadow
 * the client reads back.  An access must not straddle the two.
 */
static ssize_t bar0_access(vfu_ctx_t *vfu_ctx, char *buf, size_t count,
                           loff_t offset, bool is_write)
{
    rocm_ernic_dev_t *dev = vfu_get_private(vfu_ctx);

    if (!dev->ionic_emu) {
        vfu_log(vfu_ctx, LOG_ERR, "BAR0 access: ionic_emu is NULL!");
        errno = EFAULT;
        return -1;
    }

    if (dev->pvrdma_handle)
        pvrdma_bar0_mmio_count(dev->pvrdma_handle, is_write);

    if ((size_t)offset + count <= IONIC_BAR0_REGS_SIZE)
        return ionic_eth_emu_bar0_access(dev->ionic_emu, buf, count, offset,
                                         is_write);

    if ((size_t)offset < IONIC_BAR0_REGS_SIZE ||
        (size_t)offset + count > IONIC_BAR0_TOTAL_SIZE) {
        vfu_log(vfu_ctx, LOG_ERR,
                "ionic BAR0 access out of bounds or straddling the "
                "register/MSI-X split: offset=%#lx count=%zu",
                (unsigned long)offset, count);
        errno = EINVAL;
        return -1;
    }

    size_t msix_off = (size_t)offset - IONIC_BAR0_REGS_SIZE;
    if (is_write)
        memcpy((char *)dev->bar0_mem + msix_off, buf, count);
    else
        memcpy(buf, (char *)dev->bar0_mem + msix_off, count);
    return (ssize_t)count;
}

/**
 * BAR2 (doorbell) access callback, forwarded to ionic_eth_emu.
 */
static ssize_t bar2_access(vfu_ctx_t *vfu_ctx, char *buf, size_t count,
                           loff_t offset, bool is_write)
{
    rocm_ernic_dev_t *dev = vfu_get_private(vfu_ctx);

    if (!dev->ionic_emu) {
        vfu_log(vfu_ctx, LOG_ERR, "BAR2 access: ionic_emu is NULL!");
        errno = EFAULT;
        return -1;
    }

    if (dev->pvrdma_handle)
        pvrdma_uar_mmio_count(dev->pvrdma_handle, is_write);
    return ionic_eth_emu_bar2_access(dev->ionic_emu, buf, count, offset,
                                     is_write);
}


/**
 * Device reset callback
 *
 * Only VFU_RESET_LOST_CONN means the client disconnected. VFU_RESET_DEVICE and
 * VFU_RESET_PCI_FLR are sent by the still-connected client (guest-initiated).
 */
static int device_reset_cb(vfu_ctx_t *vfu_ctx, vfu_reset_type_t type)
{
    rocm_ernic_dev_t *dev = vfu_get_private(vfu_ctx);
    const char *conn_str = NULL;

    vfu_log(vfu_ctx, LOG_INFO, "Device reset requested (type=%u)", type);

    switch (type) {
    case VFU_RESET_DEVICE:
        /* Guest requested device reset; client still connected */
        dev->device_active = false;
        conn_str = "connected (device reset)";
        if (dev->pvrdma_handle) {
            pvrdma_inc_stats_reset_count(dev->pvrdma_handle);
        }
        break;

    case VFU_RESET_LOST_CONN:
        /* Socket/connection lost */
        vfu_log(vfu_ctx, LOG_INFO, "Client connection lost");
        dev->device_active = false;
        conn_str = "disconnected (lost connection)";
        break;

    case VFU_RESET_PCI_FLR:
        /* Guest requested PCI FLR; client still connected */
        vfu_log(vfu_ctx, LOG_INFO, "PCI FLR requested");
        dev->device_active = false;
        conn_str = "connected (PCI FLR)";
        if (dev->pvrdma_handle) {
            pvrdma_inc_stats_reset_count(dev->pvrdma_handle);
        }
        break;
    default:
        break;
    }

    if (dev->pvrdma_handle && conn_str) {
        pvrdma_set_stats_connection_state(dev->pvrdma_handle, conn_str);
        if (dev->stats_file_path) {
            pvrdma_write_stats(dev->pvrdma_handle);
        }
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
 * Initialize the shared RDMA device core via wrapper API.
 *
 * This owns the resource manager and the selected RDMA backend.  The
 * ionic front end sits in front of it; the handle is still called
 * "pvrdma" because the core is the QEMU PVRDMA port.
 */
static int pvrdma_device_init(rocm_ernic_dev_t *dev)
{
    int ret;

    /* Create PVRDMA device using wrapper API with selected backend */
    dev->pvrdma_handle = pvrdma_device_create(
        dev, dev->backend_type_str, dev->backend_device_name,
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

    ernic_startup_report(
        "RDMA device core initialized successfully with '%s' backend",
        dev->backend_type_str);

    return 0;
}

/**
 * Initialize ionic emulation layer.
 */
static int ionic_device_init(rocm_ernic_dev_t *dev)
{
    /* The ionic front end owns the registers and doorbells, but the
     * resource manager and backend behind them come from the shared core:
     * without this the admin queue has no pvrdma_handle and every
     * CREATE_CQ/CREATE_QP is a no-op stub. */
    if (pvrdma_device_init(dev) < 0)
        return -1;

    dev->ionic_emu = ionic_eth_emu_create(dev->vfu_ctx, IONIC_BAR2_DB_SIZE);
    if (!dev->ionic_emu) {
        fprintf(stderr, "ionic_device_init: failed to create eth emulator\n");
        return -1;
    }

    ionic_eth_emu_set_pvrdma(dev->ionic_emu, dev->pvrdma_handle);

    if (dev->mac_addr_set)
        ionic_eth_emu_set_mac(dev->ionic_emu, dev->mac_addr);

    dev->ionic_rdma = ionic_rdma_devcmd_create(dev->vfu_ctx);
    if (!dev->ionic_rdma) {
        ionic_eth_emu_destroy(dev->ionic_emu);
        dev->ionic_emu = NULL;
        fprintf(stderr, "ionic_device_init: failed to create rdma devcmd\n");
        return -1;
    }

    ionic_eth_emu_register_rdma_handler(
        dev->ionic_emu, ionic_rdma_devcmd_dispatch, dev->ionic_rdma);
    ionic_rdma_devcmd_set_eth_emu(dev->ionic_rdma, dev->ionic_emu);

    dev->ionic_dp = ionic_datapath_create(dev->vfu_ctx, dev->ionic_emu);
    if (!dev->ionic_dp) {
        ionic_rdma_devcmd_destroy(dev->ionic_rdma);
        ionic_eth_emu_destroy(dev->ionic_emu);
        dev->ionic_emu = NULL;
        dev->ionic_rdma = NULL;
        fprintf(stderr, "ionic_device_init: failed to create datapath\n");
        return -1;
    }

    if (!strcmp(get_backend_type_base(dev->backend_type_str), "nvmeof")) {
        struct nvmeof_target_cfg cfg;
        char err[256] = "";
        const char *opts = strchr(dev->backend_type_str, ':');

        nvmeof_target_cfg_defaults(&cfg);
        if (!nvmeof_target_cfg_parse(&cfg, opts ? opts + 1 : NULL, err,
                                     sizeof(err)) ||
            !ionic_datapath_attach_nvmeof(dev->ionic_dp, &cfg, err,
                                          sizeof(err))) {
            fprintf(stderr, "nvmeof backend: %s\n", err);
            ionic_datapath_destroy(dev->ionic_dp);
            ionic_rdma_devcmd_destroy(dev->ionic_rdma);
            ionic_eth_emu_destroy(dev->ionic_emu);
            dev->ionic_dp = NULL;
            dev->ionic_emu = NULL;
            dev->ionic_rdma = NULL;
            return -1;
        }
    }

    if (!strcmp(get_backend_type_base(dev->backend_type_str), "s3")) {
        struct s3_target_cfg cfg;
        char err[256] = "";
        const char *opts = strchr(dev->backend_type_str, ':');

        s3_target_cfg_defaults(&cfg);
        if (!s3_target_cfg_parse(&cfg, opts ? opts + 1 : NULL, err,
                                 sizeof(err)) ||
            !ionic_datapath_attach_s3(dev->ionic_dp, &cfg, err, sizeof(err))) {
            fprintf(stderr, "s3 backend: %s\n", err);
            ionic_datapath_destroy(dev->ionic_dp);
            ionic_rdma_devcmd_destroy(dev->ionic_rdma);
            ionic_eth_emu_destroy(dev->ionic_emu);
            dev->ionic_dp = NULL;
            dev->ionic_emu = NULL;
            dev->ionic_rdma = NULL;
            return -1;
        }
    }

    /* Wire the datapath into the eth emulator's BAR2 handler */
    ionic_eth_emu_register_datapath(dev->ionic_emu, dev->ionic_dp);

    dev->device_initialized = true;

    printf("ionic emulation initialized (VID:DID %#x:%#x)\n",
           (unsigned)PCI_VENDOR_ID_PENSANDO,
           (unsigned)PCI_DEVICE_ID_AMD_IONIC_ERNIC);
    return 0;
}

/**
 * Setup PCI configuration for the emulated ionic device
 */
static int setup_pci_config(vfu_ctx_t *vfu_ctx, rocm_ernic_dev_t *dev)
{
    int ret;

    /* Initialize PCI device as multi-function (Function 0 = RDMA) */
    ret =
        vfu_pci_init(vfu_ctx, VFU_PCI_TYPE_EXPRESS, PCI_HEADER_TYPE_NORMAL, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "vfu_pci_init() failed");
    }

    /* Patched ionic.ko + ionic_rdma.ko bind to this ID. */
    uint16_t did = PCI_DEVICE_ID_AMD_IONIC_ERNIC;
    vfu_pci_set_id(vfu_ctx, PCI_VENDOR_ID_PENSANDO, did, PCI_VENDOR_ID_PENSANDO,
                   did);

    /* Set PCI class code: Network Controller - Ethernet (RoCEv2) */
    vfu_pci_set_class(vfu_ctx, PCI_BASE_CLASS_NETWORK, /* Base class 0x02 */
                      0x00,  /* Subclass: Ethernet Controller */
                      0x00); /* Prog-if */

    ernic_startup_report("rocm-ernic: PCI device configured: vendor=%#x "
                         "device=%#x",
                         (unsigned)PCI_VENDOR_ID_PENSANDO, (unsigned)did);

    return 0;
}

/**
 * Setup BARs (Base Address Registers).
 *
 *   BAR0 (64-bit): 32 KB device registers (devcmd + MSI-X ctrl),
 *                  with the MSI-X table/PBA above the register window
 *   BAR2 (64-bit): 4 MB doorbell pages (BAR1 is skipped per ionic spec)
 */
static int setup_bars(vfu_ctx_t *vfu_ctx, rocm_ernic_dev_t *dev)
{
    int ret;

    /* Shadow for the MSI-X table/PBA that sits above the ionic
     * register window; the register window itself is shadowed inside
     * ionic_eth_emu. */
    dev->bar0_mem = calloc(1, IONIC_BAR0_TOTAL_SIZE - IONIC_BAR0_REGS_SIZE);
    if (!dev->bar0_mem)
        err(EXIT_FAILURE, "ionic: Failed to allocate BAR0 MSI-X shadow");

    ret = vfu_setup_region(
        vfu_ctx, VFU_PCI_DEV_BAR0_REGION_IDX, IONIC_BAR0_TOTAL_SIZE,
        bar0_access, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, NULL, 0, -1, 0);
    if (ret < 0)
        err(EXIT_FAILURE, "ionic: Failed to setup BAR0");

    ret = vfu_setup_region(
        vfu_ctx, VFU_PCI_DEV_BAR2_REGION_IDX, IONIC_BAR2_DB_SIZE, bar2_access,
        VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, NULL, 0, -1, 0);
    if (ret < 0)
        err(EXIT_FAILURE, "ionic: Failed to setup BAR2");

    ernic_startup_report(
        "rocm-ernic: ionic BARs configured: BAR0=%zu (regs=%zu) BAR2=%zu",
        (size_t)IONIC_BAR0_TOTAL_SIZE, (size_t)IONIC_BAR0_REGS_SIZE,
        (size_t)IONIC_BAR2_DB_SIZE);

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
static int setup_interrupts(vfu_ctx_t *vfu_ctx, rocm_ernic_dev_t *dev)
{
    ssize_t ret;

#define MSIX_TABLE_BIR 0 /* Table in BAR 0 */
#define MSIX_PBA_BIR   0 /* PBA in BAR 0 */

    /* Setup legacy INTx interrupt (required by some guests for PCI compliance)
     */
    ret = vfu_setup_device_nr_irqs(vfu_ctx, VFU_DEV_INTX_IRQ, 1);
    if (ret < 0) {
        vfu_log(vfu_ctx, LOG_ERR, "Failed to setup INTx interrupt: %s",
                strerror(errno));
        return (int)ret;
    }

    /* MSI-X capability structure (12 bytes total) */
    struct {
        uint8_t id;     /* Capability ID = 0x11 for MSI-X */
        uint8_t next;   /* Next capability pointer (0 = none, filled by lib) */
        uint16_t ctrl;  /* Message Control register */
        uint32_t table; /* Table Offset/BIR */
        uint32_t pba;   /* PBA Offset/BIR */
    } msix_cap;

    /* Build MSI-X capability structure */
    msix_cap.id = PCI_CAP_ID_MSIX; /* 0x11 */
    msix_cap.next =
        0; /* Will be filled by libvfio-user if there are more caps */

    /* One interrupt per Ethernet queue pair plus at least four RDMA EQs. */
    uint32_t nr_intrs = IONIC_MSIX_MAX_VECTORS;

    /* Message Control: bits [10:0] = Table Size-1 */
    msix_cap.ctrl = (uint16_t)((nr_intrs - 1u) & 0x7FFu);

    /* The table/PBA must sit above the 32 KB ionic register window: offset
     * 0 is the DEVI signature ionic_dev_setup() probes and 0x2000 is
     * intr_ctrl, so placing them at the bottom of BAR0 would alias both. */
    uint32_t table_off = IONIC_BAR0_MSIX_TABLE;
    uint32_t pba_off = IONIC_BAR0_MSIX_PBA;

    /* Table Offset/BIR: bits [2:0] = BIR, bits [31:3] = offset >> 3 */
    msix_cap.table = (table_off & 0xFFFFFFF8) | (MSIX_TABLE_BIR & 0x7);

    /* PBA Offset/BIR: bits [2:0] = BIR, bits [31:3] = offset >> 3 */
    msix_cap.pba = (pba_off & 0xFFFFFFF8) | (MSIX_PBA_BIR & 0x7);

    /* Add MSI-X capability to PCI config space at automatic position (pos=0) */
    ret = vfu_pci_add_capability(vfu_ctx, 0, 0, &msix_cap);
    if (ret < 0) {
        vfu_log(vfu_ctx, LOG_ERR, "Failed to add MSI-X capability: %s",
                strerror(errno));
        return (int)ret;
    }

    ernic_startup_report("rocm-ernic: Added MSI-X capability at offset 0x%zd",
                         ret);

    /* Ensure standard PCI header tail (0x34-0x3f) is set for config reads */
    {
        vfu_pci_config_space_t *cfg = vfu_pci_get_config_space(vfu_ctx);
        if (cfg) {
            uint8_t *p = (uint8_t *)cfg;
            p[0x34] = (uint8_t)ret; /* capability pointer */
            for (int i = 0x35; i <= 0x3f; i++) {
                p[i] = 0;
            }
        }
    }

    /* Setup interrupt vector count - libvfio-user will manage table/PBA */
    ret = vfu_setup_device_nr_irqs(vfu_ctx, VFU_DEV_MSIX_IRQ, nr_intrs);
    if (ret < 0) {
        vfu_log(vfu_ctx, LOG_ERR, "Failed to setup MSI-X IRQ count: %s",
                strerror(errno));
        return (int)ret;
    }

    ernic_startup_report("rocm-ernic: Interrupts configured: INTx=1, "
                         "MSI-X=%d vectors "
                         "(table=BAR%d:0x%x, pba=BAR%d:0x%x)",
                         (int)nr_intrs, MSIX_TABLE_BIR, (unsigned)table_off,
                         MSIX_PBA_BIR, (unsigned)pba_off);

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
    fprintf(stderr, "  -b, --backend TYPE   RDMA backend: "
                    "none|loopback|verbs|tcp|nvmeof|s3\n");
    fprintf(stderr, "                       (default: loopback)\n");
    fprintf(stderr, "  -L, --log-level LEVEL Log verbosity: "
                    "none|error|warn|info|debug\n");
    fprintf(stderr, "                       (default: warn; also settable via "
                    "ERNIC_LOG_LEVEL)\n");
    fprintf(stderr, "  -v, --verbose        Shorthand for --log-level debug\n");
    fprintf(stderr, "  -S, --stats-file PATH Statistics output file path\n");
    fprintf(stderr, "                       (stats written every ~1 second)\n");
    fprintf(stderr, "  -l, --log-file PATH  Write all output to PATH (default: "
                    "stdout/stderr)\n");
    fprintf(stderr,
            "  -m, --mac ADDRESS    MAC address (format: XX:XX:XX:XX:XX:XX)\n");
    fprintf(stderr, "                       (default: 72:6f:63:6d:2d:6e, "
                    "rocm-nic)\n");
    fprintf(stderr, "  -T, --tap IFNAME     Attach the emulated NIC to a host "
                    "TAP interface,\n");
    fprintf(stderr, "                       giving the guest working Ethernet "
                    "and TCP/IP.\n");
    fprintf(stderr, "                       Pre-create it with: ip tuntap add "
                    "dev IFNAME\n");
    fprintf(stderr, "                       mode tap user $USER\n");
    fprintf(stderr, "  -h, --help           Show this help message\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Backend Types:\n");
    fprintf(stderr, "  none: No backend (minimal stubs)\n");
    fprintf(stderr, "  loopback: Internal loopback emulation\n");
    fprintf(stderr, "                    Options (comma-separated):\n");
    fprintf(stderr,
            "                      mode=PATTERN  - Data pattern (default: "
            "preserve)\n");
    fprintf(stderr,
            "                        preserve     - Use actual guest data\n");
    fprintf(stderr, "                        zeros        - Fill with 0x00\n");
    fprintf(stderr, "                        ones         - Fill with 0xFF\n");
    fprintf(stderr, "                        increment    - Fill with "
                    "0x00,0x01,0x02,...\n");
    fprintf(stderr, "                        decrement    - Fill with "
                    "0xFF,0xFE,0xFD,...\n");
    fprintf(stderr, "                        alternate    - Fill with "
                    "0xAA,0x55,0xAA,...\n");
    fprintf(stderr,
            "                        random       - Fill with random data\n");
    fprintf(stderr,
            "                      md5           - Compute MD5 hash of data\n");
    fprintf(stderr, "                    Examples:\n");
    fprintf(
        stderr,
        "                      loopback                    - Use guest data, "
        "no MD5\n");
    fprintf(
        stderr,
        "                      loopback:mode=preserve      - Use guest data, "
        "no MD5\n");
    fprintf(stderr,
            "                      loopback:mode=random,md5    - Random data "
            "with MD5\n");
    fprintf(stderr,
            "                      loopback:mode=zeros         - All zeros, no "
            "MD5\n");
    fprintf(stderr, "  verbs: libibverbs hardware backend\n");
    fprintf(stderr, "                    Options (comma-separated):\n");
    fprintf(stderr,
            "                      device=NAME   - InfiniBand device name "
            "(required)\n");
    fprintf(stderr,
            "                      ethdev=NAME  - Ethernet device name for GID "
            "resolution\n");
    fprintf(
        stderr,
        "                      port=NUM     - IB port number (default: 1)\n");
    fprintf(stderr, "                    Examples:\n");
    fprintf(stderr,
            "                      verbs:device=mlx5_0                    - "
            "Device only\n");
    fprintf(stderr,
            "                      verbs:device=mlx5_0,ethdev=eth0         - "
            "Device and ethdev\n");
    fprintf(stderr,
            "                      verbs:device=mlx5_0,ethdev=eth0,port=1 - "
            "All options\n");
    fprintf(stderr, "  tcp: TCP/IP network backend\n");
    fprintf(stderr,
            "                    Manager Mode (centralized discovery):\n");
    fprintf(stderr, "                      tcp:manager:<ip>:<port>     - "
                    "Manager at IP:port\n");
    fprintf(stderr, "                      tcp:manager:listen:<port>    - "
                    "Manager listening on port\n");
    fprintf(stderr, "                    Worker Mode (connects to manager):\n");
    fprintf(stderr,
            "                      tcp:worker:<manager_ip>:<manager_port>\n");
    fprintf(stderr, "                    Examples:\n");
    fprintf(stderr, "                      Manager/Worker:\n");
    fprintf(stderr, "                        tcp:manager:listen:5000           "
                    "- Start manager on port 5000\n");
    fprintf(stderr, "                        tcp:worker:192.168.1.100:5000    "
                    "- Worker connects to manager\n");
    fprintf(stderr, "  nvmeof: in-process NVMe-oF controller the guest can "
                    "`nvme connect` to\n");
    fprintf(stderr, "                    Options (comma-separated):\n");
    fprintf(stderr, "                      size=BYTES   - Namespace size, "
                    "K/M/G/T suffixes (default: 64M)\n");
    fprintf(stderr, "                      file=PATH    - Back the namespace "
                    "with a file instead of RAM\n");
    fprintf(stderr,
            "                      bs=NUM       - Block size (default: 512)\n");
    fprintf(stderr, "                      nqn=NAME     - Subsystem NQN "
                    "(default: nvmet-test)\n");
    fprintf(stderr, "                      ip=ADDR      - Target address the "
                    "guest connects to\n");
    fprintf(stderr, "                                     (default: "
                    "192.168.200.1)\n");
    fprintf(
        stderr,
        "                      port=NUM     - Service id (default: 4420)\n");
    fprintf(stderr, "                      queues=NUM   - Maximum I/O queues "
                    "(default: 8)\n");
    fprintf(stderr, "                    Examples:\n");
    fprintf(stderr, "                      nvmeof                          - "
                    "64 MiB RAM namespace\n");
    fprintf(stderr, "                      nvmeof:size=1G,bs=4096          - "
                    "1 GiB, 4 KiB blocks\n");
    fprintf(stderr, "                      nvmeof:file=/tmp/ns0.img,size=1G - "
                    "File-backed namespace\n");
    fprintf(stderr, "  s3: in-process S3-over-RDMA object store the guest "
                    "reaches over HTTP\n");
    fprintf(stderr, "                    Options (comma-separated):\n");
    fprintf(stderr, "                      bucket=NAME  - Bucket name "
                    "(default: ernic)\n");
    fprintf(stderr, "                      size=BYTES   - Store capacity, "
                    "K/M/G suffixes (default: 256M)\n");
    fprintf(stderr, "                      objects=NUM  - Maximum live keys "
                    "(default: 256)\n");
    fprintf(stderr, "                      ip=ADDR      - Endpoint address the "
                    "guest talks to\n");
    fprintf(stderr, "                                     (default: "
                    "192.168.200.1)\n");
    fprintf(stderr,
            "                      port=NUM     - HTTP port (default: 9000)\n");
    fprintf(stderr, "                      maxpart=BYTES - Largest single RDMA "
                    "transfer (default: 256M)\n");
    fprintf(stderr, "                    Examples:\n");
    fprintf(stderr, "                      s3                              - "
                    "256 MiB store at 192.168.200.1:9000\n");
    fprintf(stderr, "                      s3:bucket=bench,size=2G         - "
                    "Larger store named 'bench'\n");
}

/**
 * Determine backend type from backend string
 * @backend_str: Backend string (e.g., "none", "loopback", "verbs:mlx5_0")
 * @return: Backend type string for comparison
 */
static const char *get_backend_type_base(const char *backend_str)
{
    if (!backend_str) {
        return "none";
    }
    if (!strncmp(backend_str, "loopback", 8)) {
        return "loopback";
    }
    if (!strncmp(backend_str, "verbs", 5)) {
        return "verbs";
    }
    if (!strncmp(backend_str, "tcp", 3)) {
        return "tcp";
    }
    if (!strncmp(backend_str, "nvmeof", 6)) {
        return "nvmeof";
    }
    if (!strncmp(backend_str, "s3", 2)) {
        return "s3";
    }
    return "none";
}

/**
 * Parse comma-separated verbs backend options with key=value syntax
 * Format: verbs:device=NAME[,ethdev=NAME][,port=NUM]
 * @backend_str: Backend string (e.g., "verbs:device=mlx5_0" or
 *              "verbs:device=mlx5_0,ethdev=eth0,port=1")
 * @device: Output parameter for device name (caller must free)
 * @ethdev: Output parameter for ethdev name (caller must free)
 * @port: Output parameter for port number
 * @return: 0 on success, -1 on error
 */
static int parse_verbs_options(const char *backend_str, char **device,
                               char **ethdev, uint8_t *port)
{
    const char *colon = strchr(backend_str, ':');
    if (!colon) {
        return -1;
    }

    const char *options = colon + 1;
    if (!*options) {
        return -1;
    }

    /* Parse comma-separated key=value pairs */
    char *options_copy = strdup(options);
    if (!options_copy) {
        return -1;
    }

    char *saveptr = NULL;
    char *token = strtok_r(options_copy, ",", &saveptr);

    while (token) {
        char *equals = strchr(token, '=');
        if (!equals) {
            /* Legacy format: just device name without key=value */
            if (!*device) {
                *device = strdup(token);
                if (!*device) {
                    free(options_copy);
                    return -1;
                }
            }
        } else {
            *equals = '\0';
            char *key = token;
            char *value = equals + 1;

            if (!strcmp(key, "device")) {
                if (*device) {
                    free(*device);
                }
                *device = strdup(value);
                if (!*device) {
                    free(options_copy);
                    return -1;
                }
            } else if (!strcmp(key, "ethdev")) {
                if (*ethdev) {
                    free(*ethdev);
                }
                *ethdev = strdup(value);
                if (!*ethdev) {
                    free(options_copy);
                    free(*device);
                    *device = NULL;
                    return -1;
                }
            } else if (!strcmp(key, "port")) {
                int port_val = atoi(value);
                if (port_val < 1 || port_val > 255) {
                    fprintf(stderr,
                            "Error: Invalid port number '%s' (must be "
                            "1-255)\n",
                            value);
                    free(options_copy);
                    free(*device);
                    free(*ethdev);
                    *device = NULL;
                    *ethdev = NULL;
                    return -1;
                }
                *port = (uint8_t)port_val;
            } else {
                fprintf(stderr,
                        "Warning: Unknown verbs option '%s', ignoring\n", key);
            }
        }
        token = strtok_r(NULL, ",", &saveptr);
    }

    free(options_copy);
    return 0;
}

/**
 * Validate backend-specific options
 * @dev: Device structure with parsed options
 * @return: 0 on success, -1 on error
 */
static int validate_backend_options(rocm_ernic_dev_t *dev)
{
    const char *backend_type = get_backend_type_base(dev->backend_type_str);
    bool is_verbs = !strcmp(backend_type, "verbs");

    /* For verbs backend, parse options from backend string */
    if (is_verbs) {
        char *device = NULL;
        char *ethdev = NULL;
        uint8_t port = 1;

        /* Parse comma-separated options from backend string */
        if (parse_verbs_options(dev->backend_type_str, &device, &ethdev,
                                &port) == 0) {
            /* Override with parsed values if not set via command-line */
            if (device && !dev->backend_device_name) {
                dev->backend_device_name = device;
            } else if (device) {
                free(device); /* Command-line takes precedence */
            }

            if (ethdev && !dev->backend_eth_device) {
                dev->backend_eth_device = ethdev;
            } else if (ethdev) {
                free(ethdev); /* Command-line takes precedence */
            }

            if (port != 1 && dev->backend_port_num == 1) {
                dev->backend_port_num = port;
            }
        }

        /* Device name is required for verbs backend */
        if (!dev->backend_device_name) {
            fprintf(stderr,
                    "Error: Device name required for 'verbs' backend\n");
            fprintf(stderr,
                    "  Use: --backend verbs:device=NAME[,ethdev=NAME][,port="
                    "NUM]\n");
            fprintf(stderr, "  Example: --backend verbs:device=mlx5_0\n");
            fprintf(stderr,
                    "  Example: --backend verbs:device=mlx5_0,ethdev=eth0\n");
            fprintf(stderr,
                    "  Example: --backend verbs:device=mlx5_0,ethdev=eth0,port="
                    "1\n");
            return -1;
        }
    } else {
        /* For non-verbs backends, clear any backend-specific options */
        if (dev->backend_device_name) {
            free(dev->backend_device_name);
            dev->backend_device_name = NULL;
        }
        if (dev->backend_eth_device) {
            free(dev->backend_eth_device);
            dev->backend_eth_device = NULL;
        }
        dev->backend_port_num = 1;
    }

    /* Reject a malformed namespace spec now rather than after the guest has
     * already attached and the failure looks like a device problem. */
    if (!strcmp(backend_type, "nvmeof")) {
        struct nvmeof_target_cfg cfg;
        char err[256] = "";
        const char *opts = strchr(dev->backend_type_str, ':');

        nvmeof_target_cfg_defaults(&cfg);
        if (!nvmeof_target_cfg_parse(&cfg, opts ? opts + 1 : NULL, err,
                                     sizeof(err))) {
            fprintf(stderr, "Error: nvmeof backend: %s\n", err);
            fprintf(stderr, "  Use: --backend nvmeof[:size=64M][,file=PATH]"
                            "[,bs=512][,nqn=NAME][,ip=ADDR][,port=4420]\n");
            return -1;
        }
    }

    if (!strcmp(backend_type, "s3")) {
        struct s3_target_cfg cfg;
        char err[256] = "";
        const char *opts = strchr(dev->backend_type_str, ':');

        s3_target_cfg_defaults(&cfg);
        if (!s3_target_cfg_parse(&cfg, opts ? opts + 1 : NULL, err,
                                 sizeof(err))) {
            fprintf(stderr, "Error: s3 backend: %s\n", err);
            fprintf(stderr, "  Use: --backend s3[:bucket=NAME][,size=256M]"
                            "[,objects=256][,ip=ADDR][,port=9000]\n");
            return -1;
        }
    }

    return 0;
}

/**
 * Main entry point
 */
int main(int argc, char *argv[])
{
    vfu_ctx_t *vfu_ctx;
    rocm_ernic_dev_t *dev;
    const char *socket_path = DEFAULT_SOCKET_PATH;
    const char *log_file_path = NULL;
    const char *tap_ifname = NULL;
    ErnicLogLevel log_level = ERNIC_LOG_WARN;
    bool log_level_set = false;
    struct sigaction sa;
    int ret, opt;

    /* Command-line option definitions */
    static struct option long_options[] = {
        /* Common options */
        {"socket", required_argument, 0, 's'},
        {"backend", required_argument, 0, 'b'},
        {"verbose", no_argument, 0, 'v'},
        {"log-level", required_argument, 0, 'L'},
        {"stats-file", required_argument, 0, 'S'},
        {"log-file", required_argument, 0, 'l'},
        {"mac", required_argument, 0, 'm'},
        {"help", no_argument, 0, 'h'},
        {"tap", required_argument, 0, 'T'},
        /* Backend-specific options (verbs only) */
        {"device", required_argument, 0, 'd'},
        {"ethdev", required_argument, 0, 'e'},
        {"port", required_argument, 0, 'p'},
        {0, 0, 0, 0}};

    /* Allocate device structure */
    dev = calloc(1, sizeof(*dev));
    if (!dev) {
        err(EXIT_FAILURE, "Failed to allocate device structure");
    }

    /* Set defaults */
    dev->backend_type_str =
        strdup("loopback"); /* Default to "loopback" backend */
    dev->backend_port_num = 1;
    dev->verbose = false;
    dev->device_initialized = false;
    dev->device_active = false;
    dev->mac_addr_set = false;
    /* Default MAC: 72:6f:63:6d:2d:6e (first 6 bytes of "rocm-nic" in ASCII hex)
     */
    dev->mac_addr[0] = 0x72;
    dev->mac_addr[1] = 0x6f;
    dev->mac_addr[2] = 0x63;
    dev->mac_addr[3] = 0x6d;
    dev->mac_addr[4] = 0x2d;
    dev->mac_addr[5] = 0x6e;

    /* Parse command line options */
    while ((opt = getopt_long(argc, argv, "s:b:vL:S:m:l:hT:", long_options,
                              NULL)) != -1) {
        switch (opt) {
        /* Common options */
        case 's':
            socket_path = optarg;
            break;
        case 'b':
            free(dev->backend_type_str);
            dev->backend_type_str = strdup(optarg);
            break;
        case 'v':
            dev->verbose = true;
            break;
        case 'L':
            if (!ernic_log_parse_level(optarg, &log_level)) {
                fprintf(stderr, "Error: Invalid log level: %s\n", optarg);
                fprintf(stderr,
                        "  Expected one of: none, error, warn, info, debug\n");
                free(dev->backend_type_str);
                free(dev);
                exit(EXIT_FAILURE);
            }
            log_level_set = true;
            break;
        case 'S':
            /* Store stats file path - will be set after device init */
            if (dev->stats_file_path) {
                free(dev->stats_file_path);
            }
            dev->stats_file_path = strdup(optarg);
            break;
        case 'l':
            log_file_path = optarg;
            break;
        case 'm':
            /* Parse MAC address: format XX:XX:XX:XX:XX:XX */
            {
                unsigned int mac[6];
                int count =
                    sscanf(optarg, "%02x:%02x:%02x:%02x:%02x:%02x", &mac[0],
                           &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
                if (count != 6) {
                    fprintf(stderr, "Error: Invalid MAC address format: %s\n",
                            optarg);
                    fprintf(stderr, "  Expected format: XX:XX:XX:XX:XX:XX\n");
                    fprintf(stderr, "  Example: --mac 02:00:00:00:00:01\n");
                    free(dev->backend_type_str);
                    free(dev);
                    exit(EXIT_FAILURE);
                }
                for (int i = 0; i < 6; i++) {
                    if (mac[i] > 255) {
                        fprintf(stderr, "Error: Invalid MAC address byte: %u\n",
                                mac[i]);
                        free(dev->backend_type_str);
                        free(dev);
                        exit(EXIT_FAILURE);
                    }
                    dev->mac_addr[i] = (uint8_t)mac[i];
                }
                dev->mac_addr_set = true;
            }
            break;
        case 'T':
            tap_ifname = optarg;
            break;
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    /* Log level precedence: --log-level, then --verbose, then the
     * ERNIC_LOG_LEVEL environment variable (resolved lazily), then warn. */
    if (log_level_set) {
        ernic_log_set_level(log_level);
    } else if (dev->verbose) {
        ernic_log_set_level(ERNIC_LOG_DEBUG);
    }

    /* Validate backend-specific options */
    if (validate_backend_options(dev) < 0) {
        free(dev->backend_type_str);
        free(dev->backend_device_name);
        free(dev->backend_eth_device);
        free(dev);
        exit(EXIT_FAILURE);
    }

    /* Redirect stdout and stderr to log file if requested */
    if (log_file_path) {
        int fd = open(log_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            err(EXIT_FAILURE, "Failed to open log file: %s", log_file_path);
        }
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }

    /* Setup signal handlers */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        err(EXIT_FAILURE, "Failed to setup signal handlers");
    }

    ernic_startup_report("rocm-ernic: Starting rocm-ernic device server "
                         "(Multi-Backend Support)");
    ernic_startup_report("  Socket: %s", socket_path);
    ernic_startup_report("  Backend: %s", dev->backend_type_str);
    ernic_startup_report("  Log level: %s",
                         ernic_log_level_name(ernic_log_get_level()));
    if (log_file_path) {
        ernic_startup_report("  Log file: %s", log_file_path);
    }

    /* Show backend-specific options only for verbs backend */
    if (!strcmp(get_backend_type_base(dev->backend_type_str), "verbs")) {
        if (dev->backend_device_name) {
            ernic_startup_report("  IB Device: %s", dev->backend_device_name);
        }
        if (dev->backend_eth_device) {
            ernic_startup_report("  Eth Device: %s", dev->backend_eth_device);
        }
        ernic_startup_report("  IB Port: %u", dev->backend_port_num);
    }

    /* Remove old socket if it exists - try multiple approaches */
    struct stat st;
    if (stat(socket_path, &st) == 0) {
        if (S_ISSOCK(st.st_mode)) {
            ernic_startup_report("Removing stale socket file: %s", socket_path);
            if (unlink(socket_path) != 0) {
                warn("Failed to unlink existing socket");
            }
        }
    }

    /* Give the system a moment to release the socket */
    usleep(100000); /* 100ms */

    /* Create libvfio-user context with non-blocking attach */
    vfu_ctx =
        vfu_create_ctx(VFU_TRANS_SOCK, socket_path, LIBVFIO_USER_FLAG_ATTACH_NB,
                       dev, VFU_DEV_TYPE_PCI);
    if (!vfu_ctx) {
        err(EXIT_FAILURE, "vfu_create_ctx() failed");
    }
    g_vfu_ctx = vfu_ctx;
    dev->vfu_ctx = vfu_ctx;

    /* Setup logging */
    ret = vfu_setup_log(vfu_ctx, vfu_log_cb,
                        vfu_log_threshold(ernic_log_get_level()));
    if (ret < 0) {
        err(EXIT_FAILURE, "vfu_setup_log() failed");
    }

    if (ionic_device_init(dev) < 0)
        err(EXIT_FAILURE, "ionic_device_init() failed");

    /* Set stats file path if provided */
    if (dev->stats_file_path && dev->pvrdma_handle) {
        pvrdma_set_stats_file(dev->pvrdma_handle, dev->stats_file_path);
        pvrdma_set_stats_instance_info(dev->pvrdma_handle, socket_path,
                                       dev->backend_type_str);
        pvrdma_set_stats_pci_ids(dev->pvrdma_handle, PCI_VENDOR_ID_PENSANDO,
                                 PCI_DEVICE_ID_AMD_IONIC_ERNIC);
        ernic_startup_report("rocm-ernic: Statistics will be written to: %s "
                             "(every ~1 second)",
                             dev->stats_file_path);
        pvrdma_write_stats(dev->pvrdma_handle);
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
#ifdef LIBVFIO_USER_MAX_DMA_REGIONS
    ret = vfu_setup_device_dma(vfu_ctx, LIBVFIO_USER_MAX_DMA_REGIONS,
                               dma_register_cb, dma_unregister_cb);
#else
    ret = vfu_setup_device_dma(vfu_ctx, dma_register_cb, dma_unregister_cb);
#endif
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
        fprintf(stderr,
                "rocm-ernic: WARNING: Failed to set socket permissions: %s\n",
                strerror(errno));
        fprintf(stderr,
                "rocm-ernic: You may need to manually run: sudo chmod 666 %s\n",
                socket_path);
    } else {
        ernic_startup_report("rocm-ernic: ✓ Socket permissions set to 0666 "
                             "(rw-rw-rw-) for %s",
                             socket_path);
    }

    /* Log MAC address if set */
    if (dev->mac_addr_set) {
        ernic_startup_report("rocm-ernic: Device MAC address: "
                             "%02x:%02x:%02x:%02x:%02x:%02x",
                             dev->mac_addr[0], dev->mac_addr[1],
                             dev->mac_addr[2], dev->mac_addr[3],
                             dev->mac_addr[4], dev->mac_addr[5]);
    }

    /* Attach the host network backend before the first client shows up, so a
     * bad --tap is a startup failure rather than a silently dead link. */
    if (tap_ifname) {
        if (!dev->ionic_emu) {
            fprintf(stderr,
                    "rocm-ernic: Ethernet emulation is not initialized\n");
            exit(EXIT_FAILURE);
        }
        char assigned[64] = {0};
        ret = ionic_eth_emu_attach_tap(dev->ionic_emu, tap_ifname, assigned,
                                       sizeof(assigned));
        if (ret < 0) {
            fprintf(stderr, "rocm-ernic: failed to attach TAP '%s': %s\n",
                    tap_ifname, strerror(-ret));
            fprintf(stderr,
                    "rocm-ernic: pre-create it with: ip tuntap add dev "
                    "%s mode tap user $USER\n",
                    tap_ifname);
            exit(EXIT_FAILURE);
        }
        ernic_startup_report("rocm-ernic: Ethernet attached to TAP %s",
                             assigned);
    }

    ernic_startup_report("rocm-ernic: Device realized, waiting for client "
                         "connection...");

    /* Main loop */
    while (!g_shutdown_requested) {
        /* Attach to client (non-blocking) */
        ret = vfu_attach_ctx(vfu_ctx);
        if (ret < 0) {
            if (errno == EAGAIN) {
                /* No client yet, sleep and retry */
                usleep(100000); /* 100ms */
                continue;
            } else if (errno == EINTR) {
                /* Interrupted by signal, check shutdown flag */
                continue;
            }
            vfu_log(vfu_ctx, LOG_ERR,
                    "vfu_attach_ctx() failed with errno=%d: %s", errno,
                    strerror(errno));
            err(EXIT_FAILURE, "vfu_attach_ctx() failed");
        }

        ernic_startup_report("rocm-ernic: Client connected!");
        if (dev->pvrdma_handle) {
            pvrdma_set_stats_connection_state(dev->pvrdma_handle, "connected");
        }

        /* Run device - process requests from client */
        int loop_count = 0;
        time_t last_stats_write = 0;
        GMainContext *main_context = g_main_context_default();
        /* Acquire the main context so we can iterate it */
        g_main_context_push_thread_default(main_context);
        while (!g_shutdown_requested) {
            /* Write stats periodically (every ~1 second) */
            if (dev->stats_file_path && dev->pvrdma_handle) {
                time_t now = time(NULL);
                if (now > last_stats_write) {
                    pvrdma_write_stats(dev->pvrdma_handle);
                    last_stats_write = now;
                }
            }

            /* Debug logging disabled - too verbose */
            ret = vfu_run_ctx(vfu_ctx);
            loop_count++;

            /* Process GLib idle callbacks (for WQE continuation) */
            /* Always iterate once (non-blocking) to process idle callbacks */
            /* Idle sources may not show up in g_main_context_pending() */
            gboolean had_events = g_main_context_iteration(main_context, FALSE);

            if (dev->pvrdma_handle)
                pvrdma_drain_pending_interrupts(dev->pvrdma_handle);

            /* ionic: move frames from the host TAP into the guest Rx ring.
             * This has to happen on this thread: only it may DMA. */
            if (dev->ionic_emu)
                ionic_eth_emu_poll_rx(dev->ionic_emu);

            /* ionic: poll admin queue rings for new WQEs */
            if (dev->ionic_rdma) {
                struct ionic_adminq_ctx *aqctx =
                    ionic_rdma_devcmd_get_adminq_ctx(dev->ionic_rdma);
                if (aqctx) {
                    ionic_adminq_set_pvrdma(aqctx, dev->pvrdma_handle);
                    /* Wire adminq into eth_emu so AQ doorbells update
                     * the producer index for correct poll-loop termination. */
                    if (dev->ionic_emu)
                        ionic_eth_emu_register_adminq(dev->ionic_emu, aqctx);
                    /* CREATE_CQ/QP/MR describe the guest rings the data path
                     * later needs, so it has to be reachable from here. */
                    ionic_adminq_set_datapath(aqctx, dev->ionic_dp);
                    ionic_rdma_devcmd_set_datapath(dev->ionic_rdma,
                                                   dev->ionic_dp);
                    ionic_adminq_poll(aqctx, vfu_ctx);
                }
                if (dev->ionic_dp) {
                    ionic_datapath_set_pvrdma(dev->ionic_dp,
                                              dev->pvrdma_handle);
                    /* Apply anything peer instances sent us; like the Rx poll
                     * above, this has to run on the DMA-capable thread. */
                    ionic_datapath_poll(dev->ionic_dp);
                }
            }

            if (ret < 0) {
                if (errno == ENOTCONN) {
                    ernic_startup_report(
                        "rocm-ernic: Client disconnected after %d loops",
                        loop_count);
                    if (dev->pvrdma_handle) {
                        pvrdma_set_stats_connection_state(
                            dev->pvrdma_handle, "disconnected (client closed)");
                        if (dev->stats_file_path) {
                            pvrdma_write_stats(dev->pvrdma_handle);
                        }
                    }
                    break;
                } else if (errno == EINTR) {
                    /* Interrupted by signal */
                    vfu_log(vfu_ctx, LOG_INFO,
                            "Interrupted by signal after %d loops", loop_count);
                    break;
                } else {
                    vfu_log(vfu_ctx, LOG_ERR,
                            "vfu_run_ctx() failed after %d loops: %s",
                            loop_count, strerror(errno));
                    break;
                }
            }

            /*
             * Yield briefly when idle.  100 us keeps the
             * completion-to-interrupt latency tight while
             * still avoiding 100 % CPU in the idle case.
             */
            if (ret == 0 && !had_events &&
                !ionic_datapath_has_work(dev->ionic_dp)) {
                usleep(100);
            }
        }
        vfu_log(vfu_ctx, LOG_INFO, ">>> Event loop exited after %d iterations",
                loop_count);
        /* Release the main context */
        g_main_context_pop_thread_default(main_context);
    }

    ernic_startup_report("rocm-ernic: Shutting down");

    /* Mark cleanup in progress to prevent signal handler re-entry */
    g_cleanup_in_progress = 1;

    /* Write stats before cleanup */
    if (dev->pvrdma_handle) {
        pvrdma_write_stats(dev->pvrdma_handle);
    }

    /* Disable signal handlers during cleanup */
    struct sigaction sa_ignore;
    memset(&sa_ignore, 0, sizeof(sa_ignore));
    sa_ignore.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa_ignore, NULL);
    sigaction(SIGTERM, &sa_ignore, NULL);

    /* Cleanup */
    vfu_destroy_ctx(vfu_ctx);
    g_vfu_ctx = NULL;

    /* Destroy PVRDMA device */
    if (dev->pvrdma_handle) {
        pvrdma_device_destroy(dev->pvrdma_handle);
        dev->pvrdma_handle = NULL;
    }


    free(dev->stats_file_path);

    free(dev->bar0_mem);
    free(dev->backend_device_name);
    free(dev->backend_eth_device);
    free(dev->backend_type_str);
    free(dev);

    unlink(socket_path);

    ernic_startup_report("rocm-ernic: Shutdown complete");

    return EXIT_SUCCESS;
}
