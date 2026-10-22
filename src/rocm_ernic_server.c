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
#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/xattr.h>
#include <syslog.h>
#include <getopt.h>
#include <inttypes.h>
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
#include "ionic_eth_emu.h"
#include "ionic_fault.h"
#include "ionic_rdma_devcmd.h"
#include "ionic_profile.h"
#include "nvmeof_target.h"
#include "s3_target.h"
#ifdef ERNIC_HAVE_P4
#include "p4_engine.h"
#include "p4_port.h"
#endif
#include "parse_int.h"
#include "rocm-ernic-warnings.h"

static const char *get_backend_type_base(const char *backend_str);

/* PCI identity presented over vfio-user.
 *
 * We emulate an ionic-protocol NIC, so we claim the Pensando Systems
 * vendor ID and the upstream ionic PF device ID.  The registered
 * 1dd8:5400 subsystem tuple distinguishes rocm-ernic from physical DSC
 * hardware in pci.ids output. */
#define PCI_VENDOR_ID_PENSANDO           0x1dd8
#define PCI_DEVICE_ID_AMD_IONIC_ERNIC    0x1002
#define PCI_SUBDEVICE_ID_AMD_IONIC_ERNIC 0x5400

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
 * Did vfu_attach_ctx() fail because of the client rather than the server?
 *
 * libvfio-user accepts the connection and then negotiates the protocol
 * version.  If the peer hangs up (ENOMSG, ECONNRESET, EPIPE), gives up
 * before the accept completes (ECONNABORTED), or sends something that is
 * not a valid version message (EINVAL, EPROTO), it closes the accepted
 * socket and leaves the context ready to attach again.  One bad client
 * must not take the server down with it.
 */
static bool attach_error_is_peer(int errnum)
{
    switch (errnum) {
    case ENOMSG:
    case ECONNRESET:
    case EPIPE:
    case ECONNABORTED:
    case EINVAL:
    case EPROTO:
        return true;
    default:
        return false;
    }
}

/**
 * Install ``handler`` (or SIG_IGN / SIG_DFL) for ``signo`` with no flags
 * and an empty mask.  Returns sigaction()'s result.
 */
static int set_signal_handler(int signo, void (*handler)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    /*
     * glibc defines sa_handler as a macro that expands to a member of the
     * same name, which clang reports as a recursive macro expansion.
     */
    ROCM_ERNIC_WARN_SELF_REFERENTIAL_MACRO_OFF
    sa.sa_handler = handler;
    ROCM_ERNIC_WARN_SELF_REFERENTIAL_MACRO_ON
    sigemptyset(&sa.sa_mask);

    return sigaction(signo, &sa, NULL);
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

#ifdef ERNIC_HAVE_P4
/*
 * Read a compiled P4 program off disk and hand it to the engine.
 *
 * It goes in through the same chunked path a control plane uses rather
 * than a shortcut of its own, so the one route into the engine is
 * exercised by every caller and there is no second way for a program to
 * arrive.
 */
static int p4_load_program_file(struct p4_engine *eng, const char *path)
{
    uint8_t buf[65536];
    FILE *f = fopen(path, "rbe");
    size_t total = 0;
    long size;
    int rc;

    if (!f) {
        fprintf(stderr, "rocm-ernic: cannot open P4 program '%s': %s\n", path,
                strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "rocm-ernic: cannot size P4 program '%s'\n", path);
        fclose(f);
        return -1;
    }
    if ((uint64_t)size == 0 || (uint64_t)size > P4_PROGRAM_MAX) {
        fprintf(stderr,
                "rocm-ernic: P4 program '%s' is %ld bytes; the limit is %u\n",
                path, size, P4_PROGRAM_MAX);
        fclose(f);
        return -1;
    }

    rc = p4_engine_load_begin(eng, (size_t)size);
    while (rc == 0 && total < (size_t)size) {
        size_t n = fread(buf, 1, sizeof(buf), f);

        if (n == 0) {
            rc = -EIO;
            break;
        }
        rc = p4_engine_load_chunk(eng, total, buf, n);
        total += n;
    }
    fclose(f);

    if (rc == 0)
        rc = p4_engine_load_commit(eng);

    if (rc != 0) {
        const char *why = p4_engine_load_error(eng);

        fprintf(stderr, "rocm-ernic: P4 program '%s' was refused: %s\n", path,
                why ? why : strerror(-rc));
        return -1;
    }
    return 0;
}
#endif /* ERNIC_HAVE_P4 */

/**
 * Tear down the ionic emulation layer built by ionic_device_init().
 *
 * The datapath goes first: it unregisters its mesh receive callback from
 * the pvrdma backend, and both it and the rdma devcmd hold pointers into
 * the eth emulator.  Safe to call on a partly built or already torn-down
 * device; every pointer is cleared.
 */
static void ionic_device_fini(rocm_ernic_dev_t *dev)
{
#ifdef ERNIC_HAVE_P4
    /*
     * One machine-parseable line, so a black-box test can tell a frame the
     * pipeline forwarded from one it dropped.  The counters live inside the
     * process and have no other way out.
     */
    if (dev->p4_port || dev->p4_engine) {
        struct p4_stats es;
        struct p4_port_stats ps;

        p4_engine_get_stats(dev->p4_engine, &es);
        p4_port_get_stats(dev->p4_port, &ps);
        ernic_startup_report(
            "rocm-ernic: P4 stats: host_in=%" PRIu64 " host_out=%" PRIu64
            " uplink_in=%" PRIu64 " uplink_out=%" PRIu64 " dropped=%" PRIu64
            " too_big=%" PRIu64 " parse_errors=%" PRIu64 " to_guest=%" PRIu64
            " to_net=%" PRIu64 " to_upstream=%" PRIu64
            " egress_errors=%" PRIu64,
            es.frames_in[P4_PORT_HOST], es.frames_out[P4_PORT_HOST],
            es.frames_in[P4_PORT_UPLINK], es.frames_out[P4_PORT_UPLINK],
            es.frames_dropped, es.frames_too_big, es.parse_errors, ps.to_guest,
            ps.to_net, ps.to_upstream, ps.egress_errors);
    }

    /* Before the datapath, which clears the Tx filter slot the P4 port is
     * holding on the S3 endpoint's behalf. */
    p4_port_detach(dev->p4_port);
    dev->p4_port = NULL;
    /* And before the engine, which the data path only borrowed. */
    ionic_datapath_set_p4_engine(dev->ionic_dp, NULL);
    p4_engine_destroy(dev->p4_engine);
    dev->p4_engine = NULL;
#endif
    ionic_datapath_destroy(dev->ionic_dp);
    dev->ionic_dp = NULL;
    ionic_rdma_devcmd_destroy(dev->ionic_rdma);
    dev->ionic_rdma = NULL;
    ionic_eth_emu_destroy(dev->ionic_emu);
    dev->ionic_emu = NULL;
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
    ionic_eth_emu_set_profile(dev->ionic_emu, &dev->profile);
    ionic_eth_emu_set_faults(dev->ionic_emu, dev->faults);

    dev->ionic_rdma = ionic_rdma_devcmd_create(dev->vfu_ctx);
    if (!dev->ionic_rdma) {
        ionic_device_fini(dev);
        fprintf(stderr, "ionic_device_init: failed to create rdma devcmd\n");
        return -1;
    }

    ionic_eth_emu_register_rdma_handler(
        dev->ionic_emu, ionic_rdma_devcmd_dispatch, dev->ionic_rdma);
    ionic_rdma_devcmd_set_eth_emu(dev->ionic_rdma, dev->ionic_emu);

    dev->ionic_dp = ionic_datapath_create(dev->vfu_ctx, dev->ionic_emu);
    if (!dev->ionic_dp) {
        ionic_device_fini(dev);
        fprintf(stderr, "ionic_device_init: failed to create datapath\n");
        return -1;
    }

    if (!strcmp(get_backend_type_base(dev->backend_type_str), "nvmeof")) {
        struct nvmeof_target_cfg cfg;
        char err[256] = "";
        const char *opts = strchr(dev->backend_type_str, ':');

        bool attached;

        nvmeof_target_cfg_defaults(&cfg);
        attached =
            nvmeof_target_cfg_parse(&cfg, opts ? opts + 1 : NULL, err,
                                    sizeof(err)) &&
            ionic_datapath_attach_nvmeof(dev->ionic_dp, &cfg, err, sizeof(err));
        free(cfg.file); /* the target keeps its own copy */
        if (!attached) {
            fprintf(stderr, "nvmeof backend: %s\n", err);
            ionic_device_fini(dev);
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
            ionic_device_fini(dev);
            return -1;
        }
    }

    /* Wire the datapath into the eth emulator's BAR2 handler */
    ionic_eth_emu_register_datapath(dev->ionic_emu, dev->ionic_dp);

    dev->device_initialized = true;

    printf(
        "ionic emulation initialized (VID:DID %#x:%#x, SSVID:SDID %#x:%#x)\n",
        (unsigned)PCI_VENDOR_ID_PENSANDO,
        (unsigned)PCI_DEVICE_ID_AMD_IONIC_ERNIC,
        (unsigned)PCI_VENDOR_ID_PENSANDO,
        (unsigned)PCI_SUBDEVICE_ID_AMD_IONIC_ERNIC);
    return 0;
}

/**
 * Setup PCI configuration for the emulated ionic device.  Returns 0, or -1
 * after saying why.
 */
static int setup_pci_config(vfu_ctx_t *vfu_ctx, rocm_ernic_dev_t *dev)
{
    int ret;

    /* Initialize PCI device as multi-function (Function 0 = RDMA) */
    ret =
        vfu_pci_init(vfu_ctx, VFU_PCI_TYPE_EXPRESS, PCI_HEADER_TYPE_NORMAL, 0);
    if (ret < 0) {
        warn("vfu_pci_init() failed");
        return -1;
    }

    /* Upstream ionic.ko + ionic_rdma.ko bind to this ID. */
    uint16_t did = PCI_DEVICE_ID_AMD_IONIC_ERNIC;
    vfu_pci_set_id(vfu_ctx, PCI_VENDOR_ID_PENSANDO, did, PCI_VENDOR_ID_PENSANDO,
                   PCI_SUBDEVICE_ID_AMD_IONIC_ERNIC);

    /* Set PCI class code: Network Controller - Ethernet (RoCEv2) */
    vfu_pci_set_class(vfu_ctx, PCI_BASE_CLASS_NETWORK, /* Base class 0x02 */
                      0x00,  /* Subclass: Ethernet Controller */
                      0x00); /* Prog-if */

    ernic_startup_report("rocm-ernic: PCI device configured: vendor=%#x "
                         "device=%#x subsystem=%#x:%#x",
                         (unsigned)PCI_VENDOR_ID_PENSANDO, (unsigned)did,
                         (unsigned)PCI_VENDOR_ID_PENSANDO,
                         (unsigned)PCI_SUBDEVICE_ID_AMD_IONIC_ERNIC);

    return 0;
}

/**
 * Setup BARs (Base Address Registers).
 *
 *   BAR0 (64-bit): 32 KB device registers (devcmd + MSI-X ctrl),
 *                  with the MSI-X table/PBA above the register window
 *   BAR2 (64-bit): 4 MB doorbell pages (BAR1 is skipped per ionic spec)
 *
 * Returns 0, or -1 after saying why.
 */
static int setup_bars(vfu_ctx_t *vfu_ctx, rocm_ernic_dev_t *dev)
{
    int ret;

    /* Shadow for the MSI-X table/PBA that sits above the ionic
     * register window; the register window itself is shadowed inside
     * ionic_eth_emu. */
    dev->bar0_mem = calloc(1, IONIC_BAR0_TOTAL_SIZE - IONIC_BAR0_REGS_SIZE);
    if (!dev->bar0_mem) {
        warn("ionic: Failed to allocate BAR0 MSI-X shadow");
        return -1;
    }

    ret = vfu_setup_region(
        vfu_ctx, VFU_PCI_DEV_BAR0_REGION_IDX, IONIC_BAR0_TOTAL_SIZE,
        bar0_access, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, NULL, 0, -1, 0);
    if (ret < 0) {
        warn("ionic: Failed to setup BAR0");
        return -1;
    }

    ret = vfu_setup_region(
        vfu_ctx, VFU_PCI_DEV_BAR2_REGION_IDX, IONIC_BAR2_DB_SIZE, bar2_access,
        VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM, NULL, 0, -1, 0);
    if (ret < 0) {
        warn("ionic: Failed to setup BAR2");
        return -1;
    }

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

    /* The profile's vector count: the driver budgets 1 (adminq) + one per
     * Ethernet queue pair + one per RDMA EQ, and halves to fit if short. */
    uint32_t nr_intrs = ionic_profile_intr_vectors(&dev->profile);

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

/* Milliseconds on a clock that never jumps, for the stats publish tick. */
static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
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
    fprintf(stderr, "  -G, --socket-group GROUP Let this group, a name or a "
                    "gid, connect to the\n");
    fprintf(stderr, "                       socket (mode 0660); without it, "
                    "only its owner\n");
    fprintf(stderr, "                       and root can (mode 0600)\n");
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
    fprintf(stderr, "      --nic-profile NAME|PATH  Shape the NIC from a "
                    "profile (repeatable;\n");
    fprintf(stderr, "                       later layers win). NAME is a "
                    "built-in profile\n");
    fprintf(stderr, "                       (%s); PATH an ini file.\n",
            ionic_profile_builtin_names());
    fprintf(stderr, "      --print-profile  Print the merged NIC profile as "
                    "ini and exit\n");
    fprintf(stderr, "      --stats-interval-ms MS  How often the guest's "
                    "statistics are\n");
    fprintf(stderr,
            "                       rewritten, %u-%u (default: the "
            "profile's\n",
            IONIC_PROFILE_STATS_INTERVAL_MIN, IONIC_PROFILE_STATS_INTERVAL_MAX);
    fprintf(stderr, "                       stats.interval_ms, 1000). Beats "
                    "every profile.\n");
    fprintf(stderr, "      --eth-queues N   Ethernet Rx/Tx queues offered to "
                    "the guest,\n");
    fprintf(stderr,
            "                       %u-%u (default: the profile's "
            "queues.eth, 4).\n",
            IONIC_PROFILE_ETH_QUEUES_MIN, IONIC_PROFILE_ETH_QUEUES_MAX);
    fprintf(stderr, "                       Beats every profile; the guest "
                    "uses at most\n");
    fprintf(stderr, "                       one per vCPU.\n");
    fprintf(stderr, "      --inject FAULT   Inject a fault (repeatable), as "
                    "'SITE: PATTERN effect EFFECT'\n");
    fprintf(stderr, "                       e.g. 'stats_vals: every 100 effect "
                    "fail status eio'\n");
    fprintf(stderr, "                       Patterns: every N, after N "
                    "[count M], burst N every M,\n");
    fprintf(stderr, "                       sequence \"1x3,0x10\". Sites "
                    "and effects: stats_vals fail\n");
    fprintf(stderr, "                       [status S]; eth_rx drop, nobuf; "
                    "eth_tx drop\n");
#ifdef ERNIC_HAVE_P4
    fprintf(stderr, "  -P, --p4             Put the P4 match-action engine on "
                    "the emulated wire\n");
    fprintf(stderr, "                       (see docs/p4.rst)\n");
    fprintf(stderr,
            "  -J, --p4-program PATH Load a compiled P4 program (the JSON "
            "that\n");
    fprintf(stderr, "                       p4c --target bmv2 --arch v1model "
                    "emits).\n");
    fprintf(stderr, "                       Implies --p4.\n");
#endif
    fprintf(stderr, "  -h, --help           Show this help message\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Backend Types:\n");
    fprintf(stderr, "  none: No backend (minimal stubs)\n");
    fprintf(stderr, "  loopback: Internal loopback emulation\n");
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
 * @return: 0 on success; -1 on error, with *device and *ethdev freed and
 *          set to NULL
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
                    goto fail;
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
                    goto fail;
                }
            } else if (!strcmp(key, "ethdev")) {
                if (*ethdev) {
                    free(*ethdev);
                }
                *ethdev = strdup(value);
                if (!*ethdev) {
                    goto fail;
                }
            } else if (!strcmp(key, "port")) {
                int port_val;
                if (!ernic_parse_int(value, &port_val) || port_val < 1 ||
                    port_val > 255) {
                    fprintf(stderr,
                            "Error: Invalid port number '%s' (must be "
                            "1-255)\n",
                            value);
                    goto fail;
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

fail:
    /* Hand back nothing, so the caller has nothing to free. */
    free(options_copy);
    free(*device);
    free(*ethdev);
    *device = NULL;
    *ethdev = NULL;
    return -1;
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
            } else {
                free(device); /* Command-line takes precedence */
            }

            if (ethdev && !dev->backend_eth_device) {
                dev->backend_eth_device = ethdev;
            } else {
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

        bool parsed;

        nvmeof_target_cfg_defaults(&cfg);
        parsed = nvmeof_target_cfg_parse(&cfg, opts ? opts + 1 : NULL, err,
                                         sizeof(err));
        free(cfg.file); /* only checking the options here */
        if (!parsed) {
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

/* A numeric --socket-group is parsed as a uint32_t, which only fits a gid_t
 * of that width. */
_Static_assert(sizeof(gid_t) == sizeof(uint32_t) && (gid_t)-1 > 0,
               "gid_t must be an unsigned 32-bit type");

/*
 * Resolve --socket-group: a group name or, failing that, a numeric gid, as
 * chgrp(1) does.  Any gid but (gid_t)-1 is valid, including those above
 * INT_MAX; that one tells lchown() to leave the group alone.  Returns false
 * if it's neither.
 */
static bool resolve_group(const char *spec, gid_t *gid)
{
    long max = sysconf(_SC_GETGR_R_SIZE_MAX);
    size_t len = max > 0 ? (size_t)max : 1024;
    uint32_t n;

    /* The buffer getgrnam_r() needs has no fixed bound, so grow it until
     * the entry fits, up to a limit no real group comes near. */
    for (;;) {
        struct group grp, *found = NULL;
        char *buf = malloc(len);
        int rc;

        if (!buf)
            return false;
        rc = getgrnam_r(spec, &grp, buf, len, &found);
        if (rc == 0 && found) {
            *gid = found->gr_gid;
            free(buf);
            return true;
        }
        free(buf);
        if (rc != ERANGE || len >= ((size_t)1 << 20))
            break;
        len *= 2;
    }
    if (ernic_parse_u32(spec, &n) && n != (uint32_t)(gid_t)-1) {
        *gid = (gid_t)n;
        return true;
    }
    return false;
}

/*
 * Give the socket at @path the group @gid (@name, for messages), then mode
 * 0660 to let that group in.  Prints why and returns false if it can't.
 */
static bool socket_set_group(const char *path, const char *name, gid_t gid)
{
    if (lchown(path, (uid_t)-1, gid) < 0) {
        int e = errno;

        fprintf(stderr, "rocm-ernic: cannot give socket %s the group %s: %s\n",
                path, name, strerror(e));
        if (e == EPERM)
            fprintf(stderr, "rocm-ernic: the server has to run as root or "
                            "be a member of that group\n");
        return false;
    }
    if (chmod(path, 0660) < 0) {
        fprintf(stderr, "rocm-ernic: cannot make socket %s mode 0660: %s\n",
                path, strerror(errno));
        return false;
    }
    return true;
}

/*
 * Check that exactly the socket's owner, and root, can connect to it, plus
 * the group @want_gid if it isn't NULL: mode 0600, or 0660 with that group,
 * and no access ACL.  Whatever is at @path is checked as it is, without
 * following a symlink.  Prints why and returns false if anyone else may be
 * able to connect, if the owner or group may not be able to, or if it can't
 * tell.  On success *st holds what was checked.
 *
 * The umask the socket was created under only lets the owner in, and the
 * kernel applies it to a UNIX socket itself, so the mode check is a
 * backstop: a default ACL on the directory can only take bits away.  The ACL
 * is the hole.  A default ACL is copied onto the socket, and a named entry
 * in it lets that user or group connect whatever the mode says.
 */
static bool socket_access_ok(const char *path, const gid_t *want_gid,
                             struct stat *st)
{
    mode_t want_mode = want_gid ? 0660 : 0600;

    if (lstat(path, st) < 0) {
        fprintf(stderr, "rocm-ernic: cannot stat socket %s: %s\n", path,
                strerror(errno));
        return false;
    }
    if (!S_ISSOCK(st->st_mode)) {
        fprintf(stderr, "rocm-ernic: %s is not a socket\n", path);
        return false;
    }
    if ((st->st_mode & 07777) != want_mode) {
        fprintf(stderr,
                "rocm-ernic: socket %s is mode %04o, not %04o; a default ACL "
                "on its directory can do that, so remove it (setfacl -k) or "
                "use another directory\n",
                path, (unsigned)(st->st_mode & 07777), (unsigned)want_mode);
        return false;
    }
    if (want_gid && st->st_gid != *want_gid) {
        fprintf(stderr, "rocm-ernic: socket %s has group %u, not %u\n", path,
                (unsigned)st->st_gid, (unsigned)*want_gid);
        return false;
    }

    /* A file whose ACL says no more than its mode has no ACL stored, and a
     * file system without ACLs can't have one. */
    if (lgetxattr(path, "system.posix_acl_access", NULL, 0) >= 0) {
        fprintf(stderr,
                "rocm-ernic: socket %s has an ACL, from a default ACL on its "
                "directory, that may let other users connect; remove the "
                "default ACL (setfacl -k) or use another directory, and let "
                "QEMU in with --socket-group\n",
                path);
        return false;
    }
    if (errno != ENODATA && errno != ENOTSUP) {
        fprintf(stderr, "rocm-ernic: cannot check the ACL of socket %s: %s\n",
                path, strerror(errno));
        return false;
    }
    return true;
}

/*
 * Remove a socket left at @path by a server that didn't exit cleanly, so
 * vfu_create_ctx() can bind there.  Anything else at @path, a symlink
 * included, is left alone.  Returns true if nothing is at @path now.  Prints
 * why and returns false if something other than a socket is there, or if it
 * can't tell or can't remove it.
 *
 * The check and the removal both name the entry relative to its directory,
 * opened once, so changing a component of @path in between can't point the
 * removal at another directory.  The entry itself can still be replaced in
 * between, but only by someone who can write to that directory, who could
 * remove it anyway.
 *
 * The directory is opened with O_PATH, which needs no permission on the
 * directory itself, as binding the socket doesn't: O_RDONLY would refuse one
 * the server can write and search but not read.  fstatat() and unlinkat()
 * still check search and write permission.
 */
static bool remove_stale_socket(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    /* The directory keeps its trailing slash, so "/x.sock" opens "/". */
    char *dir = slash ? strndup(path, (size_t)(slash - path) + 1) : strdup(".");
    struct stat st;
    bool ok = false;
    int dfd = -1;

    if (!dir) {
        fprintf(stderr, "rocm-ernic: out of memory\n");
        return false;
    }

    if (*name == '\0') {
        fprintf(stderr, "rocm-ernic: socket path \"%s\" has no file name\n",
                path);
    } else if ((dfd = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC)) < 0) {
        fprintf(stderr, "rocm-ernic: cannot open socket directory %s: %s\n",
                dir, strerror(errno));
    } else if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT) {
            ok = true;
        } else {
            fprintf(stderr, "rocm-ernic: cannot check %s: %s\n", path,
                    strerror(errno));
        }
    } else if (!S_ISSOCK(st.st_mode)) {
        fprintf(stderr,
                "rocm-ernic: %s exists and isn't a socket; not removing it\n",
                path);
    } else if (unlinkat(dfd, name, 0) < 0 && errno != ENOENT) {
        fprintf(stderr, "rocm-ernic: cannot remove stale socket %s: %s\n", path,
                strerror(errno));
    } else {
        ernic_startup_report("Removed stale socket file: %s", path);
        ok = true;
    }

    if (dfd >= 0) {
        close(dfd);
    }
    free(dir);
    return ok;
}

/**
 * Free the device structure and the strings it owns.  Safe on a structure
 * that was only partly filled in.
 */
static void dev_free(rocm_ernic_dev_t *dev)
{
    free(dev->stats_file_path);
    ionic_fault_set_free(dev->faults);
    free(dev->bar0_mem);
    free(dev->backend_device_name);
    free(dev->backend_eth_device);
    free(dev->backend_type_str);
    free(dev);
}

/**
 * Tear down everything built on vfu_ctx, and free the device.
 *
 * This stops and joins the backend's threads, such as the TCP backend's, so
 * the process never exits with them still running: exit() isn't thread
 * safe.  Safe on a device that was only partly built.
 */
static void server_teardown(rocm_ernic_dev_t *dev, vfu_ctx_t *vfu_ctx)
{
    /* The ionic layer first, while the ctx it was built on and the pvrdma
     * backend it registered with are both still alive. */
    ionic_device_fini(dev);

    /* Then the PVRDMA device, and the ctx last. */
    if (dev->pvrdma_handle) {
        pvrdma_device_destroy(dev->pvrdma_handle);
        dev->pvrdma_handle = NULL;
    }

    vfu_destroy_ctx(vfu_ctx);
    g_vfu_ctx = NULL;

    dev_free(dev);
}

/**
 * Main entry point
 *
 * Until vfu_ctx exists, no other thread does either, so a failure ends the
 * process at once.  From then on, a failure goes to fail, which tears the
 * device down first, and with it the threads ionic_device_init() starts.
 */
int main(int argc, char *argv[])
{
    vfu_ctx_t *vfu_ctx;
    rocm_ernic_dev_t *dev;
    const char *socket_path = DEFAULT_SOCKET_PATH;
    const char *log_file_path = NULL;
    const char *tap_ifname = NULL;
#ifdef ERNIC_HAVE_P4
    bool p4_enabled = false;
    const char *p4_program = NULL;
#endif
    const char *socket_group = NULL;
    gid_t socket_gid = 0;
    ErnicLogLevel log_level = ERNIC_LOG_WARN;
    bool log_level_set = false;
    bool print_profile = false;
    const char *stats_interval_arg = NULL;
    const char *eth_queues_arg = NULL;
    int ret, opt;

    /* Long-only options take values past the single-character ones. */
    enum {
        OPT_NIC_PROFILE = 0x100,
        OPT_PRINT_PROFILE,
        OPT_STATS_INTERVAL,
        OPT_ETH_QUEUES,
        OPT_INJECT,
    };

    /* Command-line option definitions */
    static struct option long_options[] = {
        /* Common options */
        {"socket", required_argument, NULL, 's'},
        {"socket-group", required_argument, NULL, 'G'},
        {"backend", required_argument, NULL, 'b'},
        {"verbose", no_argument, NULL, 'v'},
        {"log-level", required_argument, NULL, 'L'},
        {"stats-file", required_argument, NULL, 'S'},
        {"log-file", required_argument, NULL, 'l'},
        {"mac", required_argument, NULL, 'm'},
        {"help", no_argument, NULL, 'h'},
        {"tap", required_argument, NULL, 'T'},
        {"nic-profile", required_argument, NULL, OPT_NIC_PROFILE},
        {"print-profile", no_argument, NULL, OPT_PRINT_PROFILE},
        {"stats-interval-ms", required_argument, NULL, OPT_STATS_INTERVAL},
        {"eth-queues", required_argument, NULL, OPT_ETH_QUEUES},
        {"inject", required_argument, NULL, OPT_INJECT},
#ifdef ERNIC_HAVE_P4
        {"p4", no_argument, NULL, 'P'},
        {"p4-program", required_argument, NULL, 'J'},
#endif
        /* Backend-specific options (verbs only) */
        {"device", required_argument, NULL, 'd'},
        {"ethdev", required_argument, NULL, 'e'},
        {"port", required_argument, NULL, 'p'},
        {NULL, 0, NULL, 0}};

    /* Allocate device structure */
    dev = calloc(1, sizeof(*dev));
    if (!dev) {
        err(EXIT_FAILURE, "Failed to allocate device structure");
    }

    /* Set defaults */
    dev->backend_type_str =
        strdup("loopback"); /* Default to "loopback" backend */
    ionic_profile_defaults(&dev->profile);
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

    /* Parse command line options.  getopt_long() keeps its state in globals,
     * such as optind, so it isn't thread safe, but no other thread has
     * started yet */
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) */
    while ((opt = getopt_long(argc, argv,
#ifdef ERNIC_HAVE_P4
                              "s:G:b:vL:S:m:l:hT:PJ:",
#else
                              "s:G:b:vL:S:m:l:hT:",
#endif
                              long_options, NULL)) != -1) {
        switch (opt) {
        /* Common options */
        case 's':
            socket_path = optarg;
            break;
        case 'G':
            socket_group = optarg;
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
                dev_free(dev);
                return EXIT_FAILURE;
            }
            log_level_set = true;
            break;
        case 'S':
            /* Store stats file path - will be set after device init */
            free(dev->stats_file_path);
            dev->stats_file_path = strdup(optarg);
            break;
        case 'l':
            log_file_path = optarg;
            break;
        case 'm':
            if (!ernic_parse_mac(optarg, dev->mac_addr)) {
                fprintf(stderr, "Error: Invalid MAC address format: %s\n",
                        optarg);
                fprintf(stderr, "  Expected format: XX:XX:XX:XX:XX:XX\n");
                fprintf(stderr, "  Example: --mac 02:00:00:00:00:01\n");
                dev_free(dev);
                return EXIT_FAILURE;
            }
            dev->mac_addr_set = true;
            break;
        case 'T':
            tap_ifname = optarg;
            break;
        case OPT_NIC_PROFILE: {
            char profile_err[512];

            if (ionic_profile_apply(&dev->profile, optarg, profile_err,
                                    sizeof(profile_err)) != 0) {
                fprintf(stderr, "Error: --nic-profile: %s\n", profile_err);
                dev_free(dev);
                return EXIT_FAILURE;
            }
            break;
        }
        case OPT_PRINT_PROFILE:
            print_profile = true;
            break;
        case OPT_STATS_INTERVAL:
            /* Applied after every --nic-profile, which it beats. */
            stats_interval_arg = optarg;
            break;
        case OPT_ETH_QUEUES:
            /* Applied after every --nic-profile, which it beats. */
            eth_queues_arg = optarg;
            break;
        case OPT_INJECT: {
            char fault_err[256];

            if (!dev->faults)
                dev->faults = ionic_fault_set_new();
            if (!dev->faults ||
                ionic_fault_set_add(dev->faults, optarg, fault_err,
                                    sizeof(fault_err)) != 0) {
                fprintf(stderr, "Error: --inject '%s': %s\n", optarg,
                        dev->faults ? fault_err : "out of memory");
                dev_free(dev);
                return EXIT_FAILURE;
            }
            break;
        }
#ifdef ERNIC_HAVE_P4
        case 'P':
            p4_enabled = true;
            break;
        case 'J':
            p4_program = optarg;
            /* A program is useless without the engine on the wire, and
             * asking for one plainly means wanting the other. */
            p4_enabled = true;
            break;
#endif
        case 'h':
            usage(argv[0]);
            dev_free(dev);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            dev_free(dev);
            return EXIT_FAILURE;
        }
    }

    if (stats_interval_arg) {
        char interval_err[256];

        if (ionic_profile_set_stats_interval(
                &dev->profile, stats_interval_arg, "--stats-interval-ms",
                interval_err, sizeof(interval_err)) != 0) {
            fprintf(stderr, "Error: %s\n", interval_err);
            usage(argv[0]);
            dev_free(dev);
            return EXIT_FAILURE;
        }
    }

    if (eth_queues_arg) {
        char queues_err[256];

        if (ionic_profile_set_eth_queues(&dev->profile, eth_queues_arg,
                                         "--eth-queues", queues_err,
                                         sizeof(queues_err)) != 0) {
            fprintf(stderr, "Error: %s\n", queues_err);
            usage(argv[0]);
            dev_free(dev);
            return EXIT_FAILURE;
        }
    }

    if (print_profile) {
        int need = ionic_profile_format(&dev->profile, NULL, 0);
        char *text = need >= 0 ? malloc((size_t)need + 1) : NULL;

        if (!text ||
            ionic_profile_format(&dev->profile, text, (size_t)need + 1) < 0) {
            fprintf(stderr, "Error: --print-profile: out of memory\n");
            free(text);
            dev_free(dev);
            return EXIT_FAILURE;
        }
        fputs(text, stdout);
        free(text);
        dev_free(dev);
        return EXIT_SUCCESS;
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
        dev_free(dev);
        return EXIT_FAILURE;
    }

    /* Redirect stdout and stderr to log file if requested */
    if (log_file_path) {
        /* dup2() clears close-on-exec on the copies, so stdout and stderr
         * are still inherited.  With _FORTIFY_SOURCE, glibc 2.40 and later
         * give Clang an open(path, flags, mode) overload, and
         * android-cloexec-open looks for O_CLOEXEC in its mode */
        /* NOLINTBEGIN(android-cloexec-open) */
        int fd =
            open(log_file_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        /* NOLINTEND(android-cloexec-open) */
        if (fd < 0) {
            err(EXIT_FAILURE, "Failed to open log file: %s", log_file_path);
        }
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }

    /* Resolved before the socket exists, so a group that can't be resolved
     * fails here rather than leaving a socket QEMU can't reach; and after
     * the log is open, so the reason is in it. */
    if (socket_group && !resolve_group(socket_group, &socket_gid)) {
        fprintf(stderr, "Error: --socket-group: no such group: %s\n",
                socket_group);
        dev_free(dev);
        return EXIT_FAILURE;
    }

    /* Setup signal handlers */
    if (set_signal_handler(SIGINT, signal_handler) == -1 ||
        set_signal_handler(SIGTERM, signal_handler) == -1) {
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
    ernic_startup_report("  NIC profile: %s (hash 0x%016" PRIx64 ")",
                         dev->profile.chain, ionic_profile_hash(&dev->profile));
    ernic_startup_report("  NIC queues: %" PRIu32 " Ethernet, %" PRIu32
                         " RDMA EQs, %" PRIu32 " MSI-X vectors",
                         dev->profile.queues.eth, dev->profile.queues.rdma_eqs,
                         ionic_profile_intr_vectors(&dev->profile));
    {
        char warning[512];

        if (ionic_profile_warning(&dev->profile, warning, sizeof(warning)))
            ernic_startup_report("  NIC profile warning: %s", warning);
    }
    for (int site = 0; site < IONIC_FAULT_SITE_COUNT; site++) {
        const struct ionic_fault *f =
            ionic_fault_set_get(dev->faults, (enum ionic_fault_site)site);
        char spec[256];

        if (f && ionic_fault_format(f, spec, sizeof(spec)) >= 0)
            ernic_startup_report("  Fault: %s", spec);
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

#ifdef ERNIC_HAVE_P4
    /*
     * Ahead of the socket, because a program that will be refused must not
     * leave a listening socket behind while it is being read.  Whoever
     * connects in that window gets a server that is about to exit, having
     * seen nothing to say so; the socket appearing is the only readiness
     * signal there is.  The program is read once, here, and the engine
     * carries it to the attach below -- reading it again later would be a
     * check-then-act on a path, which is its own problem.
     */
    if (p4_enabled) {
        dev->p4_engine = p4_engine_create();
        if (!dev->p4_engine) {
            fprintf(stderr, "rocm-ernic: failed to create the P4 engine\n");
            dev_free(dev);
            return EXIT_FAILURE;
        }
        if (p4_program &&
            p4_load_program_file(dev->p4_engine, p4_program) != 0) {
            p4_engine_destroy(dev->p4_engine);
            dev->p4_engine = NULL;
            dev_free(dev);
            return EXIT_FAILURE;
        }
    }
#endif

    /* Binding fails if anything is at the path, so remove a socket that a
     * server that didn't exit cleanly left there. */
    if (!remove_stale_socket(socket_path)) {
        dev_free(dev);
        return EXIT_FAILURE;
    }

    /* Create libvfio-user context with non-blocking attach.  It binds and
     * listens on the socket here, creating it with mode 0777 less the umask.
     * Whoever connects drives the device as the guest's QEMU, and connecting
     * needs write permission, so the socket is created 0600, for its owner
     * alone, under a umask that takes the rest away.  Narrowing the mode
     * afterwards would be too late: a connection queued in between would be
     * kept.  --socket-group widens it to 0660 below, once the group is the
     * one asked for.  The umask is per process, so this must stay ahead of
     * the first thread, and it's restored so the log and stats files get the
     * caller's. */
    mode_t old_umask = umask(S_IXUSR | S_IRWXG | S_IRWXO);
    vfu_ctx =
        vfu_create_ctx(VFU_TRANS_SOCK, socket_path, LIBVFIO_USER_FLAG_ATTACH_NB,
                       dev, VFU_DEV_TYPE_PCI);
    umask(old_umask);
    if (!vfu_ctx) {
        err(EXIT_FAILURE, "vfu_create_ctx() failed");
    }

    /* Check the socket as created: a default ACL on the directory can still
     * let others in, or keep the owner out.  Refuse to serve it then;
     * destroying the context closes the listener, so a connection already
     * queued on it is never accepted.
     *
     * With --socket-group, then give the socket that group, and only then
     * let the group in, so no other group can connect even briefly; and
     * check it again, as QEMU will find it.  Both changes act on the path,
     * which is safe only because nobody untrusted can replace entries in the
     * socket's directory: one they could write to, unless it's sticky like
     * /tmp, would let them swap in a socket of their own anyway.  (A shared
     * directory, sticky or not, lets them take the name before the server
     * starts, which is why docs/usage.rst says to keep the socket out of
     * one.)  The second check would catch a path that no longer leads to
     * this socket, but only after the change. */
    struct stat sock_st;

    if (!socket_access_ok(socket_path, NULL, &sock_st) ||
        (socket_group &&
         (!socket_set_group(socket_path, socket_group, socket_gid) ||
          !socket_access_ok(socket_path, &socket_gid, &sock_st)))) {
        goto fail;
    }
    ernic_startup_report("rocm-ernic: ✓ Socket permissions set to %04o, "
                         "group %u, for %s",
                         (unsigned)(sock_st.st_mode & 07777),
                         (unsigned)sock_st.st_gid, socket_path);
    g_vfu_ctx = vfu_ctx;
    dev->vfu_ctx = vfu_ctx;

    /* Setup logging */
    ret = vfu_setup_log(vfu_ctx, vfu_log_cb,
                        vfu_log_threshold(ernic_log_get_level()));
    if (ret < 0) {
        warn("vfu_setup_log() failed");
        goto fail;
    }

    if (ionic_device_init(dev) < 0) {
        warn("ionic_device_init() failed");
        goto fail;
    }

    /* Set stats file path if provided */
    if (dev->stats_file_path && dev->pvrdma_handle) {
        pvrdma_set_stats_file(dev->pvrdma_handle, dev->stats_file_path);
        pvrdma_set_stats_instance_info(dev->pvrdma_handle, socket_path,
                                       dev->backend_type_str);
        pvrdma_set_stats_pci_ids(dev->pvrdma_handle, PCI_VENDOR_ID_PENSANDO,
                                 PCI_DEVICE_ID_AMD_IONIC_ERNIC);
        char profile_line[IONIC_PROFILE_CHAIN_MAX + 32];
        snprintf(profile_line, sizeof(profile_line),
                 "%s (hash 0x%016" PRIx64 ")", dev->profile.chain,
                 ionic_profile_hash(&dev->profile));
        pvrdma_set_stats_nic_profile(dev->pvrdma_handle, profile_line);
        if (dev->ionic_emu)
            pvrdma_set_stats_section(dev->pvrdma_handle,
                                     ionic_eth_emu_write_stats, dev->ionic_emu);
        ernic_startup_report("rocm-ernic: Statistics will be written to: %s "
                             "(every ~1 second)",
                             dev->stats_file_path);
        pvrdma_write_stats(dev->pvrdma_handle);
    }

    /* Setup PCI configuration and BARs, which say why they failed */
    if (setup_pci_config(vfu_ctx, dev) < 0 || setup_bars(vfu_ctx, dev) < 0) {
        goto fail;
    }

    /* Setup interrupts */
    if (setup_interrupts(vfu_ctx, dev) < 0) {
        warn("setup_interrupts() failed");
        goto fail;
    }

    /* Setup DMA callbacks */
#ifdef LIBVFIO_USER_MAX_DMA_REGIONS
    ret = vfu_setup_device_dma(vfu_ctx, LIBVFIO_USER_MAX_DMA_REGIONS,
                               dma_register_cb, dma_unregister_cb);
#else
    ret = vfu_setup_device_dma(vfu_ctx, dma_register_cb, dma_unregister_cb);
#endif
    if (ret < 0) {
        warn("vfu_setup_device_dma() failed");
        goto fail;
    }

    /* Setup reset callback */
    ret = vfu_setup_device_reset_cb(vfu_ctx, device_reset_cb);
    if (ret < 0) {
        warn("vfu_setup_device_reset_cb() failed");
        goto fail;
    }

    /* Realize the device */
    ret = vfu_realize_ctx(vfu_ctx);
    if (ret < 0) {
        warn("vfu_realize_ctx() failed");
        goto fail;
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
            goto fail;
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
            goto fail;
        }
        ernic_startup_report("rocm-ernic: Ethernet attached to TAP %s",
                             assigned);
    }

#ifdef ERNIC_HAVE_P4
    /* The engine and its program were made above, before the socket.  The
     * attach waits until here, after the TAP and the S3 endpoint, so the
     * engine sees a complete wire and can chain to whatever already holds
     * the Tx filter. */
    if (p4_enabled) {
        if (!dev->ionic_emu) {
            fprintf(stderr,
                    "rocm-ernic: Ethernet emulation is not initialized\n");
            goto fail;
        }

        struct p4_port_upstream upstream;
        memset(&upstream, 0, sizeof(upstream));
        ionic_eth_emu_get_tx_filter(dev->ionic_emu, &upstream.fn,
                                    &upstream.ctx);

        dev->p4_port = p4_port_attach(dev->ionic_emu, dev->p4_engine,
                                      upstream.fn ? &upstream : NULL);
        if (!dev->p4_port) {
            fprintf(stderr, "rocm-ernic: failed to attach the P4 engine\n");
            goto fail;
        }

        /* The RDMA path shares the same engine, so one program governs
         * both planes; it borrows the pointer and is given it back below. */
        ionic_datapath_set_p4_engine(dev->ionic_dp, dev->p4_engine);

        if (p4_program) {
            ernic_startup_report("rocm-ernic: P4 engine attached, running "
                                 "'%s'",
                                 p4_program);
        } else {
            ernic_startup_report("rocm-ernic: P4 engine attached (no program "
                                 "loaded; frames forwarded unchanged)");
        }
    }
#endif

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
            } else if (attach_error_is_peer(errno)) {
                ernic_startup_report("rocm-ernic: client dropped during "
                                     "negotiation (%s); waiting for the next "
                                     "client",
                                     strerror(errno));
                continue;
            }
            vfu_log(vfu_ctx, LOG_ERR,
                    "vfu_attach_ctx() failed with errno=%d: %s", errno,
                    strerror(errno));
            warn("vfu_attach_ctx() failed");
            goto fail;
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

            /* ionic: move frames from the host TAP into the guest Rx ring.
             * This has to happen on this thread: only it may DMA. */
            if (dev->ionic_emu) {
                ionic_eth_emu_poll_rx(dev->ionic_emu);
                ionic_eth_emu_publish_stats(dev->ionic_emu, monotonic_ms());
            }

            /* ionic: poll admin queue rings for new WQEs */
            if (dev->ionic_rdma) {
                struct ionic_adminq_ctx *aqctx =
                    ionic_rdma_devcmd_get_adminq_ctx(dev->ionic_rdma);
                if (aqctx) {
                    ionic_adminq_set_pvrdma(aqctx, dev->pvrdma_handle);
                    ionic_adminq_set_faults(aqctx, dev->faults);
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
    /* What each fault saw, so a test can match the guest's counters. */
    for (int site = 0; site < IONIC_FAULT_SITE_COUNT; site++) {
        const struct ionic_fault *f =
            ionic_fault_set_get(dev->faults, (enum ionic_fault_site)site);

        if (f)
            ernic_startup_report(
                "  Fault %s: eligible %" PRIu64 ", fired %" PRIu64,
                ionic_fault_site_name(f->site), f->eligible, f->fired);
    }

    /* Mark cleanup in progress to prevent signal handler re-entry */
    g_cleanup_in_progress = 1;

    /* Write stats before cleanup */
    if (dev->pvrdma_handle) {
        pvrdma_write_stats(dev->pvrdma_handle);
    }

    /* Disable signal handlers during cleanup */
    set_signal_handler(SIGINT, SIG_IGN);
    set_signal_handler(SIGTERM, SIG_IGN);

    /* Destroying vfu_ctx removes the socket.  Removing it by name again could
     * remove one that another server has since created there. */
    server_teardown(dev, vfu_ctx);

    ernic_startup_report("rocm-ernic: Shutdown complete");

    return EXIT_SUCCESS;

fail:
    /* Destroying vfu_ctx also removes the socket.  Only a clean shutdown
     * says "Shutdown complete". */
    server_teardown(dev, vfu_ctx);
    return EXIT_FAILURE;
}
