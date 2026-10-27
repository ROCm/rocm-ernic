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

#include <stdint.h>
#include <stdbool.h>
#include <vfio-user/libvfio-user.h>

#include "rocm_ernic_compat.h"
#include "ionic/ionic_eth_emu.h"
#include "ionic/ionic_rdma_devcmd.h"
#include "ionic/ionic_datapath.h"
#include "ionic/ionic_profile.h"

/* Forward declarations */
typedef struct rocm_ernic_dev rocm_ernic_dev_t;
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

    /* ionic emulation layer -- the device the guest actually sees */
    struct ionic_eth_emu *ionic_emu;
    struct ionic_rdma_devcmd_state *ionic_rdma;
    struct ionic_datapath *ionic_dp;

#ifdef ERNIC_HAVE_P4
    /* P4 match-action stage between the guest and the host network */
    struct p4_engine *p4_engine;
    /* The guest's command surface onto it, NULL unless the operator
     * granted the guest a scope with --p4-guest-scope. */
    struct p4_devcmd *p4_devcmd;
    struct p4_port *p4_port;
#endif

    /* BAR memory backing stores */
    void *bar0_mem; /* MSI-X table/PBA shadow, above the ionic register block */

    /* Backend device configuration */
    char *backend_type_str; /* Backend and options: loopback, nvmeof:size=1G */
    /* The backend backend_type_str names, from pvrdma_backend_name(): "none",
     * "loopback", "tcp", ...  Set by the server's option validation */
    const char *backend_name;

    /* Device state flags */
    bool verbose; /* Verbose logging enabled */
    /*
     * The client went and libvfio-user reset the context, unmapping all
     * guest memory.  Set by device_reset_cb(), inside vfu_run_ctx(), and
     * cleared on attach; only the main thread touches it.
     */
    bool client_lost;

    /* Statistics */
    char *stats_file_path; /* Path to stats output file */

    /* Faults from --inject, or NULL (ionic_fault.h) */
    struct ionic_fault_set *faults;

    /* MAC address */
    uint8_t mac_addr[6]; /* Device MAC address */
    bool mac_addr_set;   /* Whether MAC address was explicitly set */

    /* NIC identity and link: compat plus each --nic-profile layer */
    struct ionic_profile profile;
};

#endif /* ROCM_ERNIC_INTERNAL_H */
