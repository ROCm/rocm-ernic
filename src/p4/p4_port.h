/*
 * p4_port.h — attach the P4 engine to the emulated NIC's two ports
 *
 * The engine itself is frames-in/frames-out and knows nothing about the
 * device.  This is the part that does: it registers on both of the ionic
 * Ethernet emulator's frame filters, so every frame the guest transmits and
 * every frame the host backend receives goes through the pipeline, and it
 * maps the pipeline's egress port back onto the emulator -- the host port to
 * the guest's Rx ring, the uplink port to the host network backend.
 *
 * Everything runs on the thread that owns the vfio-user context: the Tx
 * filter is called from the doorbell path and the Rx filter from the
 * server's poll loop, both of which are already that thread.  No locking is
 * needed and no thread is started.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef P4_PORT_H
#define P4_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ionic_eth_emu;
struct p4_engine;
struct p4_port;

struct p4_port_stats {
    uint64_t to_guest;      /* frames injected into the guest's Rx ring */
    uint64_t to_net;        /* frames sent out of the host backend */
    uint64_t egress_errors; /* frames the emulator would not accept */
    uint64_t to_upstream;   /* Tx frames claimed by the previous filter */
};

/*
 * A filter that was already on the Tx path when the engine was attached.
 *
 * The emulator has one Tx filter slot and the S3 control plane may already
 * hold it (ionic_datapath_attach_s3()).  Rather than displace it, the P4
 * port takes the slot and offers each frame to the previous holder first,
 * preserving the "in-process endpoint gets first refusal" contract: a frame
 * addressed to the S3 endpoint is consumed there and never reaches the
 * pipeline, which is right, because that endpoint is not on the wire the
 * pipeline models.
 */
struct p4_port_upstream {
    bool (*fn)(void *ctx, const void *frame, size_t len);
    void *ctx;
};

/*
 * Create the attachment and register both filters on @emu.  @eng must
 * outlive the returned object.  @upstream may be NULL, or may name the Tx
 * filter already registered, which is then restored by p4_port_detach().
 * Returns NULL on allocation failure or if @emu or @eng is NULL.
 */
struct p4_port *p4_port_attach(struct ionic_eth_emu *emu, struct p4_engine *eng,
                               const struct p4_port_upstream *upstream);

/* Unregister the filters and free.  Safe with NULL. */
void p4_port_detach(struct p4_port *port);

void p4_port_get_stats(const struct p4_port *port, struct p4_port_stats *out);

#endif /* P4_PORT_H */
