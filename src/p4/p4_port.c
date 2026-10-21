/*
 * p4_port.c — the P4 engine's attachment to the emulated NIC
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ionic_eth_emu.h"
#include "p4_engine.h"
#include "p4_port.h"

struct p4_port {
    struct ionic_eth_emu *emu;
    struct p4_engine *eng;
    struct p4_port_upstream upstream;
    struct p4_port_stats stats;
    /*
     * Output staging.  Sized for the largest frame the engine will produce,
     * which is larger than the input frame whenever a program adds headers.
     */
    uint8_t out[P4_FRAME_MAX];
};

static void p4_port_emit(struct p4_port *port, const struct p4_result *res)
{
    int ret;

    if (res->egress_port == P4_PORT_HOST)
        ret = ionic_eth_emu_queue_rx_frame(port->emu, port->out, res->len);
    else
        ret = ionic_eth_emu_send_to_net(port->emu, port->out, res->len);

    if (ret != 0) {
        port->stats.egress_errors++;
        return;
    }

    if (res->egress_port == P4_PORT_HOST)
        port->stats.to_guest++;
    else
        port->stats.to_net++;
}

/*
 * Both filters claim every frame unconditionally.  Once the engine is on the
 * wire it owns forwarding in both directions, including the decision to send
 * a frame straight back out of the port it arrived on; letting the emulator
 * also forward what the pipeline left behind would duplicate it.
 */
static bool p4_port_frame(struct p4_port *port, uint32_t in_port,
                          const void *frame, size_t len)
{
    struct p4_result res;

    if (p4_engine_process(port->eng, in_port, frame, len, port->out,
                          sizeof(port->out), &res) != 0)
        return true;

    if (res.verdict == P4_VERDICT_FORWARD && res.len > 0)
        p4_port_emit(port, &res);

    return true;
}

static bool p4_port_tx_filter(void *ctx, const void *frame, size_t len)
{
    struct p4_port *port = ctx;

    if (port->upstream.fn &&
        port->upstream.fn(port->upstream.ctx, frame, len)) {
        port->stats.to_upstream++;
        return true;
    }

    return p4_port_frame(port, P4_PORT_HOST, frame, len);
}

static bool p4_port_rx_filter(void *ctx, const void *frame, size_t len)
{
    return p4_port_frame(ctx, P4_PORT_UPLINK, frame, len);
}

struct p4_port *p4_port_attach(struct ionic_eth_emu *emu, struct p4_engine *eng,
                               const struct p4_port_upstream *upstream)
{
    if (!emu || !eng)
        return NULL;

    struct p4_port *port = calloc(1, sizeof(*port));
    if (!port)
        return NULL;

    port->emu = emu;
    port->eng = eng;
    if (upstream)
        port->upstream = *upstream;

    ionic_eth_emu_register_tx_filter(emu, p4_port_tx_filter, port);
    ionic_eth_emu_register_rx_filter(emu, p4_port_rx_filter, port);

    return port;
}

void p4_port_detach(struct p4_port *port)
{
    if (!port)
        return;

    ionic_eth_emu_register_tx_filter(port->emu, port->upstream.fn,
                                     port->upstream.ctx);
    ionic_eth_emu_register_rx_filter(port->emu, NULL, NULL);

    free(port);
}

void p4_port_get_stats(const struct p4_port *port, struct p4_port_stats *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (port)
        *out = port->stats;
}
