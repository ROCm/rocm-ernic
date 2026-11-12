/*
 * Internal Device Structure for ROCm ERNIC (Emulated RDMA NIC)
 *
 * This header defines our main device structure without including QEMU headers.
 * We use opaque handles to hide QEMU types.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ROCM_ERNIC_INTERNAL_H
#define ROCM_ERNIC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <vfio-user/libvfio-user.h>

#include "rocm_ernic_compat.h"
#include "ernic_log.h"
#include "ionic/ionic_profile.h"
#include "net/net_headers.h"

/* Forward declarations */
typedef struct rocm_ernic_dev rocm_ernic_dev_t;
struct ernic_stats;
struct ionic_device;
struct ionic_fault_set;

/**
 * rocm_ernic_dev - Main device structure
 *
 * This structure contains both the libvfio-user context and a handle to
 * the RDMA device implementation. The actual device structures are
 * hidden behind the opaque pvrdma_handle_t.  The BAR layout is in
 * ionic/ionic_bar.h.
 */
struct rocm_ernic_dev {
    /* libvfio-user context */
    vfu_ctx_t *vfu_ctx;

    /* Opaque handle to the RDMA device core (resource manager + backend) */
    pvrdma_handle_t pvrdma_handle;

    /* The ionic device in front of it -- the device the guest actually
     * sees (ionic/ionic_device.h) */
    struct ionic_device *ionic;

#ifdef ERNIC_HAVE_P4
    /* P4 match-action stage between the guest and the host network, which
     * the ionic device borrows.  Made before the socket, so a refused
     * program never leaves one behind. */
    struct p4_engine *p4_engine;
#endif

    /* BAR memory backing stores */
    void *bar0_mem; /* MSI-X table/PBA shadow, above the ionic register block */

    /*
     * The config-space registers a driver writes, as setup left them,
     * which a reset writes back (save_pci_config(), reset_pci_config()).
     * Only the main thread touches them.
     */
    struct {
        size_t px_off;           /* PCI Express capability */
        size_t msix_off;         /* MSI-X capability */
        size_t pm_off;           /* Power Management capability */
        uint16_t command;        /* PCI_COMMAND */
        uint8_t cache_line_size; /* PCI_CACHE_LINE_SIZE */
        uint8_t latency_timer;   /* PCI_LATENCY_TIMER */
        uint8_t interrupt_line;  /* PCI_INTERRUPT_LINE */
        uint16_t devctl;         /* PCI_EXP_DEVCTL */
        uint16_t devctl2;        /* PCI_EXP_DEVCTL2 */
        uint16_t lnkctl2;        /* PCI_EXP_LNKCTL2 */
        uint16_t msix_flags;     /* PCI_MSIX_FLAGS */
        uint16_t pmcsr;          /* PCI_PM_CTRL */
    } pci_init;

    /* Limits the config-space messages a client can have logged as often as
     * it writes (config_access()).  Only the main thread touches it. */
    ErnicLogLimit cfg_log_limit;

    /* Backend device configuration */
    char *backend_type_str; /* Backend and options: loopback, nvmeof:size=1G */

    /* Device state flags */
    bool verbose; /* Verbose logging enabled */
    /*
     * The client went and libvfio-user reset the context, unmapping all
     * guest memory.  Set by device_reset_cb(), inside vfu_run_ctx(), and
     * cleared on attach; only the main thread touches it.
     */
    bool client_lost;

    /* The device's counters and the stats file (ernic_stats.h), which the
     * ionic device counts into */
    struct ernic_stats *stats;
    char *stats_file_path; /* --stats-file, or NULL */

    /* Faults from --inject, or NULL (ionic_fault.h) */
    struct ionic_fault_set *faults;

    /* MAC address */
    uint8_t mac_addr[ETH_ADDR_LEN]; /* Device MAC address */
    bool mac_addr_set;              /* Whether MAC address was explicitly set */

    /* NIC identity and link: compat plus each --nic-profile layer */
    struct ionic_profile profile;

    /* --cmb: the device has a Controller Memory Buffer, BAR4 */
    bool cmb;
};

#endif /* ROCM_ERNIC_INTERNAL_H */
