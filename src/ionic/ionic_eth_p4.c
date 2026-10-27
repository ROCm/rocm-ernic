/*
 * ionic_eth_p4.c — the P4 engine's attachment to the emulated NIC's ports
 *
 * See ionic_eth_p4.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ionic/ionic_eth_p4.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ionic/ionic_eth_emu.h"
#include "ionic/ionic_eth_net.h"
#include "p4/p4_engine.h"

/*
 * The engine must accept anything the emulator can hand it: eth_p4_frame()
 * below claims every frame, so a frame the engine refuses is one the guest
 * sent and nothing forwards.
 */
_Static_assert(P4_FRAME_MAX >= IONIC_ETH_NET_MTU_MAX,
               "the engine would silently drop frames the NIC can carry");

struct ionic_eth_p4 {
    struct ionic_eth_emu *emu;
    struct p4_engine *eng;
    struct ionic_eth_p4_upstream upstream;
    struct ionic_eth_p4_stats stats;
    /*
     * Output staging.  Sized for the largest frame the engine will produce,
     * which is larger than the input frame whenever a program adds headers.
     */
    uint8_t out[P4_FRAME_MAX];
};

static void eth_p4_emit(struct ionic_eth_p4 *ep, const struct p4_result *res)
{
    int ret;

    if (res->egress_port == P4_PORT_HOST)
        ret = ionic_eth_emu_queue_rx_frame(ep->emu, ep->out, res->len);
    else
        ret = ionic_eth_emu_send_to_net(ep->emu, ep->out, res->len);

    if (ret != 0) {
        ep->stats.egress_errors++;
        return;
    }

    if (res->egress_port == P4_PORT_HOST)
        ep->stats.to_guest++;
    else
        ep->stats.to_net++;
}

/*
 * Both filters claim every frame unconditionally.  Once the engine is on the
 * wire it owns forwarding in both directions, including the decision to send
 * a frame straight back out of the port it arrived on; letting the emulator
 * also forward what the pipeline left behind would duplicate it.
 */
static bool eth_p4_frame(struct ionic_eth_p4 *ep, uint32_t in_port,
                         const void *frame, size_t len)
{
    struct p4_result res;

    if (p4_engine_process(ep->eng, in_port, frame, len, ep->out,
                          sizeof(ep->out), &res) != 0)
        return true;

    if (res.verdict == P4_VERDICT_FORWARD && res.len > 0)
        eth_p4_emit(ep, &res);

    return true;
}

static bool eth_p4_tx_filter(void *ctx, const void *frame, size_t len)
{
    struct ionic_eth_p4 *ep = ctx;

    if (ep->upstream.fn && ep->upstream.fn(ep->upstream.ctx, frame, len)) {
        ep->stats.to_upstream++;
        return true;
    }

    return eth_p4_frame(ep, P4_PORT_HOST, frame, len);
}

static bool eth_p4_rx_filter(void *ctx, const void *frame, size_t len)
{
    return eth_p4_frame(ctx, P4_PORT_UPLINK, frame, len);
}

struct ionic_eth_p4 *ionic_eth_p4_attach(
    struct ionic_eth_emu *emu, struct p4_engine *eng,
    const struct ionic_eth_p4_upstream *upstream)
{
    if (!emu || !eng)
        return NULL;

    struct ionic_eth_p4 *ep = calloc(1, sizeof(*ep));
    if (!ep)
        return NULL;

    ep->emu = emu;
    ep->eng = eng;
    if (upstream)
        ep->upstream = *upstream;

    ionic_eth_emu_register_tx_filter(emu, eth_p4_tx_filter, ep);
    ionic_eth_emu_register_rx_filter(emu, eth_p4_rx_filter, ep);

    return ep;
}

void ionic_eth_p4_detach(struct ionic_eth_p4 *ep)
{
    if (!ep)
        return;

    ionic_eth_emu_register_tx_filter(ep->emu, ep->upstream.fn,
                                     ep->upstream.ctx);
    ionic_eth_emu_register_rx_filter(ep->emu, NULL, NULL);

    free(ep);
}

void ionic_eth_p4_get_stats(const struct ionic_eth_p4 *ep,
                            struct ionic_eth_p4_stats *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (ep)
        *out = ep->stats;
}
