/*
 * ionic_device.h — the emulated ionic device, as one object
 *
 * The device the guest sees is four objects: the Ethernet emulator
 * (registers, device commands, Ethernet queues, interrupts and doorbells),
 * the RDMA devcmd handler, the admin queue context that handler creates, and
 * the data path.  This module creates them, wires them together once, polls
 * them, and tears them down in the order they need.  The RDMA core behind
 * them (pvrdma_device_create()) is the caller's, as are the statistics they
 * count into, the vfio-user context, the socket and the main loop.
 *
 * Main loop thread only: the device DMAs into guest memory.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_DEVICE_H
#define IONIC_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vfio-user/libvfio-user.h>

#include "rocm_ernic_compat.h"
#ifdef ERNIC_HAVE_P4
#include "p4_devcmd.h"
#endif

/* Opaque device state. */
struct ionic_device;

struct ionic_device_cfg {
    /* The context the device DMAs, raises interrupts and logs through */
    vfu_ctx_t *vfu_ctx;
    /* The RDMA core, from pvrdma_device_create(), realized.  It must
     * outlive the device. */
    pvrdma_handle_t pvrdma;
    /* The counters the device counts into (ernic_stats.h), or NULL for none.
     * The device adds its Ethernet counters to their stats file as a
     * section.  They must outlive the device. */
    struct ernic_stats *stats;
    /* The --backend string, e.g. "loopback" or "nvmeof:size=1G".  An
     * "nvmeof" or "s3" backend attaches that in-process target, with the
     * options after the ':'.  NULL means "none". */
    const char *backend;
    /* The NIC's identity and link (ionic_profile.h), copied; NULL for
     * compat */
    const struct ionic_profile *profile;
    /* The station MAC, copied; NULL for the emulator's default */
    const uint8_t *mac;
    /* The faults to inject (ionic_fault.h), or NULL for none.  It must
     * outlive the device. */
    struct ionic_fault_set *faults;
};

/*
 * Create the device and attach the backend's target, if it has one.
 * Returns NULL and fills @err on failure: out of memory, or a target whose
 * options are refused or which can't be started.  Call before any client
 * connects.
 */
struct ionic_device *ionic_device_create(const struct ionic_device_cfg *cfg,
                                         char *err, size_t errlen);

/*
 * Tear the device down.  Call while the RDMA core and the vfio-user context
 * are still alive: the data path unregisters from the core, and putting
 * back guest mappings goes through the context.  Safe with NULL.
 */
void ionic_device_destroy(struct ionic_device *dev);

/*
 * Check the options of the target @backend names, as ionic_device_create()
 * would, without starting it, so that a bad one fails at startup rather
 * than looking like a device problem once a guest attaches.  Returns false
 * and fills @err if they are refused; true for a backend with no target.
 */
bool ionic_device_target_check(const char *backend, char *err, size_t errlen);

/*
 * One pass of the device's share of the main loop: move received frames
 * into the guest's Rx rings and rewrite its Ethernet statistics when they
 * are due, run new RDMA admin commands, and apply what peers sent over the
 * mesh.  Call it after vfu_run_ctx() on every pass while a client is
 * connected, and never once libvfio-user has reset the context: the guest
 * memory it would DMA to is gone.
 */
void ionic_device_poll(struct ionic_device *dev);

/*
 * True when work is waiting that the next ionic_device_poll() would do, so
 * the caller can skip its idle sleep.
 */
bool ionic_device_has_work(struct ionic_device *dev);

/*
 * The parts, for code that works on one directly: the server's BAR
 * callbacks and TAP attach use the Ethernet emulator, and the tests reach
 * into all three.  NULL for a NULL @dev.
 */
struct ionic_eth_emu *ionic_device_eth_emu(const struct ionic_device *dev);
struct ionic_datapath *ionic_device_datapath(const struct ionic_device *dev);
struct ionic_rdma_devcmd_state *ionic_device_rdma_devcmd(
    const struct ionic_device *dev);

#ifdef ERNIC_HAVE_P4
struct p4_engine;

/*
 * Put the P4 engine @eng on the emulated wire, between the guest and the
 * host network, and on the RDMA path, so one program governs both.  Call it
 * after anything else that takes the Ethernet Tx filter (a TAP, the S3
 * target), which the engine then chains to.  @guest_scope is what the guest
 * may do to the engine through its device commands; P4_SCOPE_NONE gives it
 * none.
 *
 * @eng stays the caller's and must outlive the device.  Returns false and
 * fills @err on failure; what was attached comes off with the device.
 */
bool ionic_device_attach_p4(struct ionic_device *dev, struct p4_engine *eng,
                            enum p4_scope guest_scope, char *err,
                            size_t errlen);
#endif

#endif /* IONIC_DEVICE_H */
