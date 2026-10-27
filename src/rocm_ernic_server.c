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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <syslog.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>

#include <vfio-user/libvfio-user.h>
#include <vfio-user/pci_defs.h>
#include <linux/pci_regs.h>
#include <glib.h> /* For g_main_context_iteration() */

/* Internal headers */
/*
 * struct rocm_ernic_dev.  IWYU credits uses of it to the repeated
 * rocm_ernic_dev_t typedef in rocm_ernic_compat.h, so it would drop this.
 */
#include "rocm_ernic_internal.h" // IWYU pragma: keep
#include "rocm_ernic_compat.h"
#include "ernic_socket.h"
#include "ernic_time.h"
#include "ernic_log.h"
#include "ernic_stats.h"
#include "ionic/ionic_bar.h"
#include "ionic/ionic_device.h"
#include "ionic/ionic_eth_emu.h"
#include "ionic/ionic_eth_net.h"
#include "ionic/ionic_fault.h"
#include "ionic/ionic_profile.h"
#ifdef ERNIC_HAVE_P4
#include "p4_devcmd.h"
#include "p4_engine.h"
#endif
#include "parse_int.h"
#include "rocm-ernic-warnings.h"

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

/* Flags shared with signal_handler() */
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_cleanup_in_progress = 0;

/**
 * Signal handler for graceful shutdown.  It only sets a flag, which the
 * main loop polls.
 */
static void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        if (!g_cleanup_in_progress) {
            g_shutdown_requested = 1;
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
 * Unblock SIGINT and SIGTERM in the calling thread.  Returns 0, or -1 with
 * errno set, like set_signal_handler().
 */
static int unblock_shutdown_signals(void)
{
    sigset_t set;
    int ret;

    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);

    /* pthread_sigmask() returns an error number rather than setting errno */
    ret = pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    if (ret != 0) {
        errno = ret;
        return -1;
    }
    return 0;
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
    struct ionic_eth_emu *emu = ionic_device_eth_emu(dev->ionic);

    if (!emu) {
        vfu_log(vfu_ctx, LOG_ERR, "BAR0 access: no ionic device");
        errno = EFAULT;
        return -1;
    }

    ernic_stats_bar0_access(dev->stats, is_write);

    /* The client chooses both, and offset is signed, so neither the
     * conversion nor the sum may be formed before they are known to fit. */
    if (offset < 0 || (uint64_t)offset > IONIC_BAR0_TOTAL_SIZE ||
        count > IONIC_BAR0_TOTAL_SIZE - (size_t)offset) {
        vfu_log(vfu_ctx, LOG_ERR,
                "ionic BAR0 access out of bounds: offset=%#" PRIx64
                " count=%zu",
                (uint64_t)offset, count);
        errno = EINVAL;
        return -1;
    }

    size_t off = (size_t)offset;

    if (count <= IONIC_BAR0_REGS_SIZE && off <= IONIC_BAR0_REGS_SIZE - count)
        return ionic_eth_emu_bar0_access(emu, buf, count, offset, is_write);

    if (off < IONIC_BAR0_REGS_SIZE) {
        vfu_log(vfu_ctx, LOG_ERR,
                "ionic BAR0 access straddling the register/MSI-X split: "
                "offset=%#zx count=%zu",
                off, count);
        errno = EINVAL;
        return -1;
    }

    size_t msix_off = off - IONIC_BAR0_REGS_SIZE;
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
    struct ionic_eth_emu *emu = ionic_device_eth_emu(dev->ionic);

    if (!emu) {
        vfu_log(vfu_ctx, LOG_ERR, "BAR2 access: no ionic device");
        errno = EFAULT;
        return -1;
    }

    ernic_stats_doorbell_access(dev->stats, is_write);
    return ionic_eth_emu_bar2_access(emu, buf, count, offset, is_write);
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
        conn_str = "connected (device reset)";
        ernic_stats_device_reset(dev->stats);
        break;

    case VFU_RESET_LOST_CONN:
        /* Socket/connection lost */
        vfu_log(vfu_ctx, LOG_INFO, "Client connection lost");
        dev->client_lost = true;
        conn_str = "disconnected (lost connection)";
        break;

    case VFU_RESET_PCI_FLR:
        /* Guest requested PCI FLR; client still connected */
        vfu_log(vfu_ctx, LOG_INFO, "PCI FLR requested");
        conn_str = "connected (PCI FLR)";
        ernic_stats_device_reset(dev->stats);
        break;
    default:
        break;
    }

    if (conn_str) {
        ernic_stats_set_connection(dev->stats, conn_str);
        ernic_stats_write(dev->stats);
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
    dev->pvrdma_handle = pvrdma_device_create(dev, dev->backend_type_str);

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
 * Build the device: the statistics, the RDMA core and its backend, then the
 * ionic device the guest sees in front of them, which counts into the
 * statistics.  The ionic device owns the registers and doorbells, but the
 * resource manager and backend behind them come from the core.  Returns 0,
 * or -1 after saying why.
 */
static int device_init(rocm_ernic_dev_t *dev)
{
    struct ionic_device_cfg cfg;
    char err[512] = "";

    dev->stats = ernic_stats_create();
    if (!dev->stats) {
        fprintf(stderr, "Failed to allocate the statistics\n");
        return -1;
    }

    if (pvrdma_device_init(dev) < 0)
        return -1;

    memset(&cfg, 0, sizeof(cfg));
    cfg.vfu_ctx = dev->vfu_ctx;
    cfg.pvrdma = dev->pvrdma_handle;
    cfg.stats = dev->stats;
    cfg.backend = dev->backend_type_str;
    cfg.profile = &dev->profile;
    cfg.mac = dev->mac_addr_set ? dev->mac_addr : NULL;
    cfg.faults = dev->faults;
    dev->ionic = ionic_device_create(&cfg, err, sizeof(err));
    if (!dev->ionic) {
        fprintf(stderr, "%s\n", err);
        return -1;
    }

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
static int setup_pci_config(vfu_ctx_t *vfu_ctx, const rocm_ernic_dev_t *dev)
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
static int setup_interrupts(vfu_ctx_t *vfu_ctx, const rocm_ernic_dev_t *dev)
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

    /* ret is a non-negative offset here, so the cast to size_t is exact */
    ernic_startup_report("rocm-ernic: Added MSI-X capability at offset %#zx",
                         (size_t)ret);

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
    fprintf(stderr, "  -G, --socket-group GROUP Let this group, a name or a "
                    "gid, connect to the\n");
    fprintf(stderr, "                       socket (mode 0660); without it, "
                    "only its owner\n");
    fprintf(stderr, "                       and root can (mode 0600)\n");
    fprintf(stderr, "  -b, --backend TYPE   RDMA backend: "
                    "none|loopback|tcp|nvmeof|s3\n");
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
    fprintf(stderr, "  -Y, --p4-guest-scope SCOPE  What the GUEST may do to "
                    "the engine:\n");
    fprintf(stderr, "                       none (default), read, write, "
                    "load.\n");
    fprintf(stderr, "                       'load' lets the guest replace the "
                    "program,\n");
    fprintf(stderr, "                       and so decide where every frame "
                    "goes.\n");
    fprintf(stderr, "                       Implies --p4.\n");
#endif
    fprintf(stderr, "  -h, --help           Show this help message\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Backend Types:\n");
    fprintf(stderr, "  none: No backend (minimal stubs)\n");
    fprintf(stderr, "  loopback: Internal loopback emulation\n");
    fprintf(stderr, "  tcp: TCP/IP network backend\n");
    fprintf(stderr,
            "                    Manager Mode (centralized discovery):\n");
    fprintf(stderr, "                      tcp:manager:<host>:<port>   - "
                    "Manager at host:port\n");
    fprintf(stderr, "                      tcp:manager:listen:<port>    - "
                    "Manager listening on port\n");
    fprintf(stderr, "                    Worker Mode (connects to manager):\n");
    fprintf(stderr,
            "                      tcp:worker:<manager_host>:<manager_port>\n");
    fprintf(stderr, "                    A host is a name or an IPv4 or IPv6 "
                    "address; an IPv6\n"
                    "                    address goes in brackets: "
                    "[fe80::1%%eth0]:5000\n");
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
    fprintf(stderr, "                                     (default with "
                    "file=: an existing image's size)\n");
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
 * Validate the backend and its options before anything is built, so that a
 * malformed one fails now rather than after the guest has attached, when the
 * failure would look like a device problem.
 * @dev: Device structure with parsed options
 * @return: 0 on success, -1 on error
 */
static int validate_backend_options(const rocm_ernic_dev_t *dev)
{
    const char *name = pvrdma_backend_name(dev->backend_type_str);
    char err[512] = "";

    if (!name) {
        fprintf(stderr, "Error: Unknown backend '%s'\n", dev->backend_type_str);
        fprintf(stderr, "  Expected one of: none, loopback, tcp, nvmeof, s3, "
                        "optionally followed by ':' and its options\n");
        return -1;
    }

    if (!ionic_device_target_check(dev->backend_type_str, err, sizeof(err))) {
        fprintf(stderr, "Error: %s\n", err);
        if (!strcmp(name, "nvmeof"))
            fprintf(stderr, "  Use: --backend nvmeof[:size=64M][,file=PATH]"
                            "[,bs=512][,nqn=NAME][,ip=ADDR][,port=4420]\n");
        else if (!strcmp(name, "s3"))
            fprintf(stderr, "  Use: --backend s3[:bucket=NAME][,size=256M]"
                            "[,objects=256][,ip=ADDR][,port=9000]\n");
        return -1;
    }

    return 0;
}

/**
 * Allocate the device structure, with the defaults the options override.
 * Returns NULL after saying why.
 */
static rocm_ernic_dev_t *dev_new(void)
{
    /* 72:6f:63:6d:2d:6e, the first 6 bytes of "rocm-nic" in ASCII */
    static const uint8_t default_mac[6] = {0x72, 0x6f, 0x63, 0x6d, 0x2d, 0x6e};
    rocm_ernic_dev_t *dev = calloc(1, sizeof(*dev));

    if (!dev) {
        warn("Failed to allocate device structure");
        return NULL;
    }
    dev->backend_type_str = strdup("loopback");
    if (!dev->backend_type_str) {
        fprintf(stderr, "Error: out of memory\n");
        free(dev);
        return NULL;
    }
    ionic_profile_defaults(&dev->profile);
    memcpy(dev->mac_addr, default_mac, sizeof(dev->mac_addr));
    return dev;
}

/**
 * Free the device structure and what it owns.  Safe on a structure that was
 * only partly filled in.
 */
static void dev_free(rocm_ernic_dev_t *dev)
{
#ifdef ERNIC_HAVE_P4
    p4_engine_destroy(dev->p4_engine);
#endif
    free(dev->stats_file_path);
    ionic_fault_set_free(dev->faults);
    free(dev->bar0_mem);
    free(dev->backend_type_str);
    free(dev);
}

/**
 * Tear down everything built on dev->vfu_ctx, and free the device.
 *
 * This stops and joins the backend's threads, such as the TCP backend's, so
 * the process never exits with them still running: exit() isn't thread
 * safe.  Safe on a device that was only partly built.
 */
static void server_teardown(rocm_ernic_dev_t *dev)
{
    /* The ionic device first, while the ctx it was built on and the RDMA
     * core it registered with are both still alive. */
    ionic_device_destroy(dev->ionic);
    dev->ionic = NULL;

    /* Then the RDMA core and the statistics, and the ctx last.  Destroying
     * the ctx calls device_reset_cb(), which finds no statistics, so the
     * stats file keeps what it said before shutdown. */
    if (dev->pvrdma_handle) {
        pvrdma_device_destroy(dev->pvrdma_handle);
        dev->pvrdma_handle = NULL;
    }
    ernic_stats_destroy(dev->stats);
    dev->stats = NULL;

    vfu_destroy_ctx(dev->vfu_ctx);
    dev->vfu_ctx = NULL;

    dev_free(dev);
}

/* The options that configure the server rather than the device */
struct server_opts {
    const char *socket_path;
    const char *socket_group; /* NULL lets in only the owner and root */
    gid_t socket_gid;         /* socket_group, resolved */
    const char *log_file_path;
    const char *tap_ifname;
    ErnicLogLevel log_level;
    bool log_level_set;
#ifdef ERNIC_HAVE_P4
    bool p4_enabled;
    /*
     * What the guest may do with the engine.  NONE by default: a guest
     * that can replace the P4 program decides where every frame goes and
     * which RDMA operations become fp8 reductions, and that is not a
     * capability to hand out because the operator happened to turn the
     * engine on for their own use.
     */
    enum p4_scope p4_guest_scope;
    const char *p4_program;
#endif
};

/* What main() does after parse_options() */
enum parse_result {
    PARSE_RUN,          /* Start the server */
    PARSE_EXIT_SUCCESS, /* --help or --print-profile has been answered */
    PARSE_EXIT_FAILURE, /* The options were refused, saying why */
};

/**
 * Print @profile as ini, for --print-profile.  Returns false after saying
 * why if it can't.
 */
static bool print_nic_profile(const struct ionic_profile *profile)
{
    int need = ionic_profile_format(profile, NULL, 0);
    char *text = need >= 0 ? malloc((size_t)need + 1) : NULL;

    if (!text || ionic_profile_format(profile, text, (size_t)need + 1) < 0) {
        fprintf(stderr, "Error: --print-profile: out of memory\n");
        free(text);
        return false;
    }
    fputs(text, stdout);
    free(text);
    return true;
}

/**
 * Parse the command line into @dev and @opts, which hold the defaults.
 */
static enum parse_result parse_options(int argc, char *argv[],
                                       rocm_ernic_dev_t *dev,
                                       struct server_opts *opts)
{
    const char *stats_interval_arg = NULL;
    const char *eth_queues_arg = NULL;
    bool print_profile = false;
    int opt;

    /* Long-only options take values past the single-character ones. */
    enum {
        OPT_NIC_PROFILE = 0x100,
        OPT_PRINT_PROFILE,
        OPT_STATS_INTERVAL,
        OPT_ETH_QUEUES,
        OPT_INJECT,
    };

    /* Command-line option definitions */
    static const struct option long_options[] = {
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
        {"p4-guest-scope", required_argument, NULL, 'Y'},
#endif
        {NULL, 0, NULL, 0}};

    /* getopt_long() keeps its state in globals, such as optind, so it isn't
     * thread safe, but no other thread has started yet */
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) */
    while ((opt = getopt_long(argc, argv,
#ifdef ERNIC_HAVE_P4
                              "s:G:b:vL:S:m:l:hT:PJ:Y:",
#else
                              "s:G:b:vL:S:m:l:hT:",
#endif
                              long_options, NULL)) != -1) {
        switch (opt) {
        /* Common options */
        case 's':
            opts->socket_path = optarg;
            break;
        case 'G':
            opts->socket_group = optarg;
            break;
        case 'b':
            free(dev->backend_type_str);
            dev->backend_type_str = strdup(optarg);
            if (!dev->backend_type_str) {
                fprintf(stderr, "Error: --backend: out of memory\n");
                return PARSE_EXIT_FAILURE;
            }
            break;
        case 'v':
            dev->verbose = true;
            break;
        case 'L':
            if (!ernic_log_parse_level(optarg, &opts->log_level)) {
                fprintf(stderr, "Error: Invalid log level: %s\n", optarg);
                fprintf(stderr,
                        "  Expected one of: none, error, warn, info, debug\n");
                return PARSE_EXIT_FAILURE;
            }
            opts->log_level_set = true;
            break;
        case 'S':
            /* Store stats file path - will be set after device init */
            free(dev->stats_file_path);
            dev->stats_file_path = strdup(optarg);
            if (!dev->stats_file_path) {
                fprintf(stderr, "Error: --stats-file: out of memory\n");
                return PARSE_EXIT_FAILURE;
            }
            break;
        case 'l':
            opts->log_file_path = optarg;
            break;
        case 'm':
            if (!ernic_parse_mac(optarg, dev->mac_addr)) {
                fprintf(stderr, "Error: Invalid MAC address format: %s\n",
                        optarg);
                fprintf(stderr, "  Expected format: XX:XX:XX:XX:XX:XX\n");
                fprintf(stderr, "  Example: --mac 02:00:00:00:00:01\n");
                return PARSE_EXIT_FAILURE;
            }
            dev->mac_addr_set = true;
            break;
        case 'T':
            opts->tap_ifname = optarg;
            break;
        case OPT_NIC_PROFILE: {
            char profile_err[512];

            if (ionic_profile_apply(&dev->profile, optarg, profile_err,
                                    sizeof(profile_err)) != 0) {
                fprintf(stderr, "Error: --nic-profile: %s\n", profile_err);
                return PARSE_EXIT_FAILURE;
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
                return PARSE_EXIT_FAILURE;
            }
            break;
        }
#ifdef ERNIC_HAVE_P4
        case 'P':
            opts->p4_enabled = true;
            break;
        case 'J':
            opts->p4_program = optarg;
            /* A program is useless without the engine on the wire, and
             * asking for one plainly means wanting the other. */
            opts->p4_enabled = true;
            break;
        case 'Y':
            if (!p4_scope_parse(optarg, &opts->p4_guest_scope)) {
                fprintf(stderr,
                        "rocm-ernic: --p4-guest-scope must be one of "
                        "none, read, write, load (got '%s')\n",
                        optarg);
                return PARSE_EXIT_FAILURE;
            }
            /* Granting the guest a scope without the engine would be a
             * surface onto nothing, and plainly means wanting both. */
            opts->p4_enabled = true;
            break;
#endif
        case 'h':
            usage(argv[0]);
            return PARSE_EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return PARSE_EXIT_FAILURE;
        }
    }

    if (stats_interval_arg) {
        char interval_err[256];

        if (ionic_profile_set_stats_interval(
                &dev->profile, stats_interval_arg, "--stats-interval-ms",
                interval_err, sizeof(interval_err)) != 0) {
            fprintf(stderr, "Error: %s\n", interval_err);
            usage(argv[0]);
            return PARSE_EXIT_FAILURE;
        }
    }

    if (eth_queues_arg) {
        char queues_err[256];

        if (ionic_profile_set_eth_queues(&dev->profile, eth_queues_arg,
                                         "--eth-queues", queues_err,
                                         sizeof(queues_err)) != 0) {
            fprintf(stderr, "Error: %s\n", queues_err);
            usage(argv[0]);
            return PARSE_EXIT_FAILURE;
        }
    }

    if (print_profile)
        return print_nic_profile(&dev->profile) ? PARSE_EXIT_SUCCESS
                                                : PARSE_EXIT_FAILURE;
    return PARSE_RUN;
}

/**
 * Send stdout and stderr to the --log-file @path.  Returns 0, or -1 after
 * saying why on the stderr the server was started with.
 */
static int redirect_output(const char *path)
{
    /* dup2() clears close-on-exec on the copies, so stdout and stderr are
     * still inherited.  With _FORTIFY_SOURCE, glibc 2.40 and later give
     * Clang an open(path, flags, mode) overload, and android-cloexec-open
     * looks for O_CLOEXEC in its mode */
    /* NOLINTBEGIN(android-cloexec-open) */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    /* NOLINTEND(android-cloexec-open) */

    if (fd < 0) {
        warn("Failed to open log file: %s", path);
        return -1;
    }
    /* stderr changes only if both succeed, so on failure the reason still
     * goes to the inherited one */
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        warn("Failed to redirect output to log file: %s", path);
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/**
 * Report the configuration the server starts with.
 */
static void report_startup(const rocm_ernic_dev_t *dev,
                           const struct server_opts *opts)
{
    char warning[512];

    ernic_startup_report("rocm-ernic: Starting rocm-ernic device server "
                         "(Multi-Backend Support)");
    ernic_startup_report("  Socket: %s", opts->socket_path);
    ernic_startup_report("  Backend: %s", dev->backend_type_str);
    ernic_startup_report("  Log level: %s",
                         ernic_log_level_name(ernic_log_get_level()));
    if (opts->log_file_path) {
        ernic_startup_report("  Log file: %s", opts->log_file_path);
    }
    ernic_startup_report("  NIC profile: %s (hash 0x%016" PRIx64 ")",
                         dev->profile.chain, ionic_profile_hash(&dev->profile));
    ernic_startup_report("  NIC queues: %" PRIu32 " Ethernet, %" PRIu32
                         " RDMA EQs, %" PRIu32 " MSI-X vectors",
                         dev->profile.queues.eth, dev->profile.queues.rdma_eqs,
                         ionic_profile_intr_vectors(&dev->profile));
    if (ionic_profile_warning(&dev->profile, warning, sizeof(warning)))
        ernic_startup_report("  NIC profile warning: %s", warning);
    for (int site = 0; site < IONIC_FAULT_SITE_COUNT; site++) {
        const struct ionic_fault *f =
            ionic_fault_set_get(dev->faults, (enum ionic_fault_site)site);
        char spec[256];

        if (f && ionic_fault_format(f, spec, sizeof(spec)) >= 0)
            ernic_startup_report("  Fault: %s", spec);
    }
}

#ifdef ERNIC_HAVE_P4
/**
 * Create the P4 engine and load @program into it, if it isn't NULL.
 * Returns 0, or -1 after saying why; dev_free() frees the engine.
 *
 * Ahead of the socket, because a program that will be refused must not
 * leave a listening socket behind while it is being read.  Whoever connects
 * in that window gets a server that is about to exit, having seen nothing to
 * say so; the socket appearing is the only readiness signal there is.  The
 * program is read once, here, and the engine carries it to attach_p4() --
 * reading it again later would be a check-then-act on a path, which is its
 * own problem.
 */
static int load_p4_engine(rocm_ernic_dev_t *dev, const char *program)
{
    dev->p4_engine = p4_engine_create();
    if (!dev->p4_engine) {
        fprintf(stderr, "rocm-ernic: failed to create the P4 engine\n");
        return -1;
    }
    if (program && p4_load_program_file(dev->p4_engine, program) != 0)
        return -1;
    return 0;
}
#endif

/**
 * Create the vfio-user context, with @dev as its private data, and let only
 * whom the options name connect to its socket.  Returns NULL after saying
 * why.
 */
static vfu_ctx_t *create_socket(const struct server_opts *opts,
                                rocm_ernic_dev_t *dev)
{
    const char *path = opts->socket_path;
    struct stat created_st;
    struct stat sock_st;
    vfu_ctx_t *vfu_ctx;
    mode_t old_umask;

    /* Binding fails if anything is at the path, so remove a socket that a
     * server that didn't exit cleanly left there. */
    if (!ernic_socket_remove_stale(path))
        return NULL;

    /* Create libvfio-user context with non-blocking attach.  It binds and
     * listens on the socket here, creating it with mode 0777 less the umask.
     * Whoever connects drives the device as the guest's QEMU, and connecting
     * needs write permission, so the socket is created 0600, for its owner
     * alone, under a umask that takes the rest away.  Narrowing the mode
     * afterwards would be too late: a connection queued in between would be
     * kept.  --socket-group widens it to 0660 below, once the group is the
     * one asked for.  The umask is per process, so this must stay ahead of
     * the first thread, and it's restored so the log file gets the caller's.
     * (The stats file is always 0644; see ernic_stats_write().) */
    old_umask = umask(S_IXUSR | S_IRWXG | S_IRWXO);
    vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, path, LIBVFIO_USER_FLAG_ATTACH_NB,
                             dev, VFU_DEV_TYPE_PCI);
    umask(old_umask);
    if (!vfu_ctx) {
        warn("vfu_create_ctx() failed");
        return NULL;
    }

    /* Check the socket as created: a default ACL on the directory can still
     * let others in, or keep the owner out.  Refuse to serve it then;
     * destroying the context closes the listener, so a connection already
     * queued on it is never accepted.
     *
     * With --socket-group, then give the socket that group, and only then
     * let the group in, so no other group can connect even briefly; and
     * check it again, as QEMU will find it, and that it's still the socket
     * the first check found.  Both changes act on that socket through a
     * descriptor, so whoever can replace entries in the socket's directory
     * can't point them at another file.  They could still swap in a socket
     * of their own after the second check, unless the directory is sticky
     * like /tmp.  (A shared directory, sticky or not, lets them take the
     * name before the first check, which is why docs/usage.rst says to keep
     * the socket out of one.  The first check is the server's only anchor:
     * fstat() on the listening socket describes the socket, not the file.) */
    if (!ernic_socket_access_ok(path, NULL, NULL, &created_st)) {
        vfu_destroy_ctx(vfu_ctx);
        return NULL;
    }
    sock_st = created_st;
    if (opts->socket_group &&
        (!ernic_socket_set_group(path, opts->socket_group, opts->socket_gid,
                                 &created_st) ||
         !ernic_socket_access_ok(path, &opts->socket_gid, &created_st,
                                 &sock_st))) {
        vfu_destroy_ctx(vfu_ctx);
        return NULL;
    }
    ernic_startup_report("rocm-ernic: ✓ Socket permissions set to %04o, "
                         "group %u, for %s",
                         (unsigned)(sock_st.st_mode & 07777),
                         (unsigned)sock_st.st_gid, path);
    return vfu_ctx;
}

/**
 * Point the statistics file, if there is one, at this instance, and write it
 * once.  Returns 0, or -1 after saying why.
 */
static int setup_stats_file(rocm_ernic_dev_t *dev, const char *socket_path)
{
    char profile_line[IONIC_PROFILE_CHAIN_MAX + 32];
    struct ernic_stats_instance inst;

    if (!dev->stats_file_path)
        return 0;

    snprintf(profile_line, sizeof(profile_line), "%s (hash 0x%016" PRIx64 ")",
             dev->profile.chain, ionic_profile_hash(&dev->profile));
    memset(&inst, 0, sizeof(inst));
    inst.socket_path = socket_path;
    inst.backend = dev->backend_type_str;
    inst.nic_profile = profile_line;
    inst.pci_vid = PCI_VENDOR_ID_PENSANDO;
    inst.pci_did = PCI_DEVICE_ID_AMD_IONIC_ERNIC;
    memcpy(inst.mac, dev->mac_addr, sizeof(inst.mac));
    if (ernic_stats_set_file(dev->stats, dev->stats_file_path) < 0 ||
        ernic_stats_set_instance(dev->stats, &inst) < 0) {
        fprintf(stderr, "Error: out of memory for the stats file\n");
        return -1;
    }
    ernic_startup_report("rocm-ernic: Statistics will be written to: %s "
                         "(every ~1 second)",
                         dev->stats_file_path);
    ernic_stats_write(dev->stats);
    return 0;
}

/**
 * Present the device over vfio-user: its PCI identity, BARs and interrupts,
 * and the DMA and reset callbacks; then realize the context.  Returns 0, or
 * -1 after saying why.
 */
static int setup_vfu_device(vfu_ctx_t *vfu_ctx, rocm_ernic_dev_t *dev)
{
    int ret;

    /* Setup PCI configuration and BARs, which say why they failed */
    if (setup_pci_config(vfu_ctx, dev) < 0 || setup_bars(vfu_ctx, dev) < 0) {
        return -1;
    }

    /* Setup interrupts */
    if (setup_interrupts(vfu_ctx, dev) < 0) {
        warn("setup_interrupts() failed");
        return -1;
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
        return -1;
    }

    /* Setup reset callback */
    ret = vfu_setup_device_reset_cb(vfu_ctx, device_reset_cb);
    if (ret < 0) {
        warn("vfu_setup_device_reset_cb() failed");
        return -1;
    }

    /* Realize the device */
    ret = vfu_realize_ctx(vfu_ctx);
    if (ret < 0) {
        warn("vfu_realize_ctx() failed");
        return -1;
    }

    return 0;
}

/**
 * Attach the Ethernet to the host TAP @ifname.  Returns 0, or -1 after
 * saying why.
 */
static int attach_tap(rocm_ernic_dev_t *dev, const char *ifname)
{
    char assigned[64] = {0};
    int ret = ionic_eth_emu_attach_tap(ionic_device_eth_emu(dev->ionic), ifname,
                                       assigned, sizeof(assigned));

    if (ret == -ENAMETOOLONG) {
        fprintf(stderr,
                "rocm-ernic: TAP name '%s' is too long (at most %d "
                "characters)\n",
                ifname, IONIC_ETH_NET_IFNAME_MAX);
        return -1;
    }
    if (ret < 0) {
        fprintf(stderr, "rocm-ernic: failed to attach TAP '%s': %s\n", ifname,
                strerror(-ret));
        fprintf(stderr,
                "rocm-ernic: pre-create it with: ip tuntap add dev "
                "%s mode tap user $USER\n",
                ifname);
        return -1;
    }
    ernic_startup_report("rocm-ernic: Ethernet attached to TAP %s", assigned);
    return 0;
}

#ifdef ERNIC_HAVE_P4
/**
 * Put the P4 engine load_p4_engine() made on the wire.  Returns 0, or -1
 * after saying why.
 *
 * The attach waits until the TAP and the S3 endpoint are attached, so the
 * engine sees a complete wire and can chain to whatever already holds the
 * Tx filter.  The guest's command surface onto the engine DMAs into guest
 * memory, so it needs the vfio-user context, which doesn't exist until the
 * socket is created; the program is still read before the socket.
 */
static int attach_p4(rocm_ernic_dev_t *dev, const struct server_opts *opts)
{
    char err[256] = "";

    if (!ionic_device_attach_p4(dev->ionic, dev->p4_engine,
                                opts->p4_guest_scope, err, sizeof(err))) {
        fprintf(stderr, "rocm-ernic: %s\n", err);
        return -1;
    }

    if (opts->p4_program) {
        ernic_startup_report("rocm-ernic: P4 engine attached, running '%s'",
                             opts->p4_program);
    } else {
        ernic_startup_report("rocm-ernic: P4 engine attached (no program "
                             "loaded; frames forwarded unchanged)");
    }
    ernic_startup_report("rocm-ernic: P4 guest scope: %s",
                         p4_scope_name(opts->p4_guest_scope));
    return 0;
}
#endif

/**
 * Build the device on dev->vfu_ctx and realize it, ready for a client.
 * Returns 0, or -1 after saying why.
 */
static int setup_device(rocm_ernic_dev_t *dev, const struct server_opts *opts)
{
    vfu_ctx_t *vfu_ctx = dev->vfu_ctx;

    /* Setup logging */
    if (vfu_setup_log(vfu_ctx, vfu_log_cb,
                      vfu_log_threshold(ernic_log_get_level())) < 0) {
        warn("vfu_setup_log() failed");
        return -1;
    }

    if (device_init(dev) < 0 || setup_stats_file(dev, opts->socket_path) < 0)
        return -1;

    if (setup_vfu_device(vfu_ctx, dev) < 0)
        return -1;

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
    if (opts->tap_ifname && attach_tap(dev, opts->tap_ifname) < 0)
        return -1;

#ifdef ERNIC_HAVE_P4
    if (opts->p4_enabled && attach_p4(dev, opts) < 0)
        return -1;
#endif

    return 0;
}

/**
 * Serve the client that has attached, until it goes, vfu_run_ctx() fails,
 * or shutdown is requested.
 */
static void serve_client(rocm_ernic_dev_t *dev)
{
    vfu_ctx_t *vfu_ctx = dev->vfu_ctx;
    uint64_t loop_count = 0;
    uint64_t next_stats_write_ms = 0;
    GMainContext *main_context = g_main_context_default();
    int ret;

    /* Acquire the main context so we can iterate it */
    g_main_context_push_thread_default(main_context);
    while (!g_shutdown_requested) {
        /* Write stats periodically (every ~1 second) */
        if (dev->stats_file_path) {
            uint64_t now_ms = ernic_monotonic_ms();
            if (now_ms >= next_stats_write_ms) {
                ernic_stats_write(dev->stats);
                next_stats_write_ms = now_ms + 1000u;
            }
        }

        ret = vfu_run_ctx(vfu_ctx);
        loop_count++;

        /*
         * Checked before anything else runs.  When the client goes,
         * libvfio-user resets the context, unmapping all guest memory,
         * before vfu_run_ctx() returns, so the device poll below would DMA
         * to addresses that no longer map to anything.  The return value
         * doesn't always say so: after a reset because a reply failed,
         * vfu_run_ctx() returns success.  device_reset_cb() sees every
         * reset.  The poll can also change errno, which says why
         * vfu_run_ctx() failed.
         */
        if (dev->client_lost || (ret < 0 && errno == ENOTCONN)) {
            ernic_startup_report(
                "rocm-ernic: Client disconnected after %" PRIu64 " loops",
                loop_count);
            ernic_stats_set_connection(dev->stats,
                                       "disconnected (client closed)");
            ernic_stats_write(dev->stats);
            break;
        } else if (ret < 0 && errno == EINTR) {
            /* Interrupted by signal */
            vfu_log(vfu_ctx, LOG_INFO,
                    "Interrupted by signal after %" PRIu64 " loops",
                    loop_count);
            break;
        } else if (ret < 0) {
            vfu_log(vfu_ctx, LOG_ERR,
                    "vfu_run_ctx() failed after %" PRIu64 " loops: %s",
                    loop_count, strerror(errno));
            break;
        }

        /* Process GLib idle callbacks (for WQE continuation) */
        /* Always iterate once (non-blocking) to process idle callbacks */
        /* Idle sources may not show up in g_main_context_pending() */
        gboolean had_events = g_main_context_iteration(main_context, FALSE);

        /* The device's share: frames into the guest's Rx rings, RDMA admin
         * commands, and what peers sent over the mesh.  This has to happen
         * on this thread: only it may DMA. */
        ionic_device_poll(dev->ionic);

        /*
         * Yield briefly when idle.  100 us keeps the
         * completion-to-interrupt latency tight while
         * still avoiding 100 % CPU in the idle case.
         */
        if (ret == 0 && !had_events && !ionic_device_has_work(dev->ionic)) {
            ernic_sleep_us(100);
        }
    }
    vfu_log(vfu_ctx, LOG_INFO,
            ">>> Event loop exited after %" PRIu64 " iterations", loop_count);
    /* Release the main context */
    g_main_context_pop_thread_default(main_context);
}

/**
 * Wait for a client and serve it, one after another, until shutdown is
 * requested.  Returns 0 then, or -1 if attaching fails for a reason no
 * client could have caused.
 */
static int run(rocm_ernic_dev_t *dev)
{
    vfu_ctx_t *vfu_ctx = dev->vfu_ctx;

    while (!g_shutdown_requested) {
        /* Attach to client (non-blocking) */
        if (vfu_attach_ctx(vfu_ctx) < 0) {
            if (errno == EAGAIN) {
                /*
                 * No client yet: wait for one on the listening socket.  A
                 * signal ends the wait; the timeout bounds the delay when
                 * one arrives just before poll() starts.
                 */
                struct pollfd pfd = {.fd = vfu_get_poll_fd(vfu_ctx),
                                     .events = POLLIN};

                poll(&pfd, 1, 100);
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
            return -1;
        }

        ernic_startup_report("rocm-ernic: Client connected!");
        dev->client_lost = false;
        ernic_stats_set_connection(dev->stats, "connected");

        serve_client(dev);
    }
    return 0;
}

/**
 * What each fault saw, so a test can match the guest's counters.
 */
static void report_faults(const rocm_ernic_dev_t *dev)
{
    for (int site = 0; site < IONIC_FAULT_SITE_COUNT; site++) {
        const struct ionic_fault *f =
            ionic_fault_set_get(dev->faults, (enum ionic_fault_site)site);

        if (f)
            ernic_startup_report(
                "  Fault %s: eligible %" PRIu64 ", fired %" PRIu64,
                ionic_fault_site_name(f->site), f->eligible, f->fired);
    }
}

/**
 * Main entry point
 *
 * Until the vfio-user context exists, no other thread does either, so a
 * failure ends the process at once.  From then on, a failure goes to fail,
 * which tears the device down first, and with it the backend's threads,
 * which device_init() starts.
 */
int main(int argc, char *argv[])
{
    struct server_opts opts;
    rocm_ernic_dev_t *dev;

    memset(&opts, 0, sizeof(opts));
    opts.socket_path = DEFAULT_SOCKET_PATH;
    opts.log_level = ERNIC_LOG_WARN;
#ifdef ERNIC_HAVE_P4
    opts.p4_guest_scope = P4_SCOPE_NONE;
#endif

    dev = dev_new();
    if (!dev) {
        return EXIT_FAILURE;
    }

    switch (parse_options(argc, argv, dev, &opts)) {
    case PARSE_RUN:
        break;
    case PARSE_EXIT_SUCCESS:
        dev_free(dev);
        return EXIT_SUCCESS;
    case PARSE_EXIT_FAILURE:
    default:
        dev_free(dev);
        return EXIT_FAILURE;
    }

    /* Log level precedence: --log-level, then --verbose, then the
     * ERNIC_LOG_LEVEL environment variable (resolved lazily), then warn. */
    if (opts.log_level_set) {
        ernic_log_set_level(opts.log_level);
    } else if (dev->verbose) {
        ernic_log_set_level(ERNIC_LOG_DEBUG);
    }

    if (validate_backend_options(dev) < 0 ||
        (opts.log_file_path && redirect_output(opts.log_file_path) < 0)) {
        dev_free(dev);
        return EXIT_FAILURE;
    }

    /* Resolved before the socket exists, so a group that can't be resolved
     * fails here rather than leaving a socket QEMU can't reach; and after
     * the log is open, so the reason is in it. */
    if (opts.socket_group &&
        !ernic_socket_resolve_group(opts.socket_group, &opts.socket_gid)) {
        fprintf(stderr, "Error: --socket-group: no such group: %s\n",
                opts.socket_group);
        dev_free(dev);
        return EXIT_FAILURE;
    }

    /* Setup signal handlers.  Worker threads block every signal
     * (qemu_thread_create()), so this thread takes SIGINT and SIGTERM, and
     * must not block them itself: a mask is inherited across exec, so a
     * launcher could have left them blocked.  One already pending is taken
     * by the handler once unblocked. */
    if (set_signal_handler(SIGINT, signal_handler) == -1 ||
        set_signal_handler(SIGTERM, signal_handler) == -1 ||
        unblock_shutdown_signals() == -1) {
        err(EXIT_FAILURE, "Failed to setup signal handlers");
    }

    report_startup(dev, &opts);

#ifdef ERNIC_HAVE_P4
    if (opts.p4_enabled && load_p4_engine(dev, opts.p4_program) < 0) {
        dev_free(dev);
        return EXIT_FAILURE;
    }
#endif

    dev->vfu_ctx = create_socket(&opts, dev);
    if (!dev->vfu_ctx) {
        dev_free(dev);
        return EXIT_FAILURE;
    }

    if (setup_device(dev, &opts) < 0) {
        goto fail;
    }

    ernic_startup_report("rocm-ernic: Device realized, waiting for client "
                         "connection...");

    if (run(dev) < 0) {
        goto fail;
    }

    ernic_startup_report("rocm-ernic: Shutting down");
    report_faults(dev);

    /* Mark cleanup in progress to prevent signal handler re-entry */
    g_cleanup_in_progress = 1;

    /* Write stats before cleanup */
    ernic_stats_write(dev->stats);

    /* Disable signal handlers during cleanup */
    set_signal_handler(SIGINT, SIG_IGN);
    set_signal_handler(SIGTERM, SIG_IGN);

    /* Destroying vfu_ctx removes the socket.  Removing it by name again could
     * remove one that another server has since created there. */
    server_teardown(dev);

    ernic_startup_report("rocm-ernic: Shutdown complete");

    return EXIT_SUCCESS;

fail:
    /* Destroying vfu_ctx also removes the socket.  Only a clean shutdown
     * says "Shutdown complete". */
    server_teardown(dev);
    return EXIT_FAILURE;
}
