/*
 * p4_engine.c — match-action pipeline execution
 *
 * Phase 1 of the engine: the façade, the port model, the frame bounds and
 * the statistics are real; the pipeline itself is not loaded yet, so every
 * frame takes the no-program path and is forwarded unchanged to the other
 * port.  That is deliberately byte-for-byte what the emulator did before
 * this module existed, so wiring the engine in cannot change behaviour until
 * a program is actually loaded.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "p4_engine.h"

struct p4_engine {
    /*
     * The loaded bmv2 program, or NULL.  Everything downstream of this
     * pointer arrives from a file and from control-plane messages, so it is
     * untrusted; the loader validates it once here rather than leaving each
     * use to re-check.
     */
    struct p4_program *prog;

    struct p4_stats stats;
};

struct p4_engine *p4_engine_create(void)
{
    struct p4_engine *eng = calloc(1, sizeof(*eng));
    return eng;
}

void p4_engine_destroy(struct p4_engine *eng)
{
    if (!eng)
        return;
    free(eng);
}

bool p4_engine_has_program(const struct p4_engine *eng)
{
    return eng != NULL && eng->prog != NULL;
}

/* The other end of a two-port device. */
static uint32_t p4_peer_port(uint32_t port)
{
    return port == P4_PORT_HOST ? P4_PORT_UPLINK : P4_PORT_HOST;
}

int p4_engine_process(struct p4_engine *eng, uint32_t in_port,
                      const void *frame, size_t len, void *out, size_t out_cap,
                      struct p4_result *res)
{
    if (!eng || !frame || !out || !res || len == 0)
        return -EINVAL;
    if (in_port >= P4_PORT_COUNT)
        return -EINVAL;

    if (len > P4_FRAME_MAX) {
        eng->stats.frames_too_big++;
        return -EMSGSIZE;
    }
    if (len > out_cap)
        return -ENOSPC;

    memset(res, 0, sizeof(*res));
    eng->stats.frames_in[in_port]++;

    memcpy(out, frame, len);
    res->verdict = P4_VERDICT_FORWARD;
    res->egress_port = p4_peer_port(in_port);
    res->len = len;

    eng->stats.frames_out[res->egress_port]++;
    return 0;
}

void p4_engine_get_stats(const struct p4_engine *eng, struct p4_stats *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (eng)
        *out = eng->stats;
}
