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
#include "p4_json.h"

struct p4_engine {
    /*
     * The loaded bmv2 program, or NULL.  Everything downstream of this
     * pointer arrives from a file and from control-plane messages, so it is
     * untrusted; the loader validates it once here rather than leaving each
     * use to re-check.
     */
    struct p4_program *prog;

    /*
     * The program being loaded, if one is.  @load_got is both how much has
     * arrived and the only offset the next chunk may name, which is what
     * keeps the buffer free of holes without a range map.
     */
    uint8_t *load_buf;
    size_t load_total;
    size_t load_got;
    /* Why the last load was refused, for a caller to report onwards. */
    char load_err[160];

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
    p4_engine_load_abort(eng);
    p4_json_free(eng->prog);
    free(eng);
}

const char *p4_engine_load_error(const struct p4_engine *eng)
{
    return (eng && eng->load_err[0]) ? eng->load_err : NULL;
}

bool p4_engine_has_program(const struct p4_engine *eng)
{
    return eng != NULL && eng->prog != NULL;
}

void p4_engine_load_abort(struct p4_engine *eng)
{
    if (!eng)
        return;
    free(eng->load_buf);
    eng->load_buf = NULL;
    eng->load_total = 0;
    eng->load_got = 0;
}

int p4_engine_load_begin(struct p4_engine *eng, size_t total_len)
{
    if (!eng)
        return -EINVAL;
    if (total_len == 0 || total_len > P4_PROGRAM_MAX)
        return -EINVAL;

    /* Whatever was part-way through is gone; this is the only place a
     * second load is allowed to disturb a first. */
    p4_engine_load_abort(eng);

    eng->load_buf = malloc(total_len);
    if (!eng->load_buf)
        return -ENOMEM;
    eng->load_total = total_len;
    eng->load_got = 0;
    return 0;
}

int p4_engine_load_chunk(struct p4_engine *eng, uint64_t offset,
                         const void *data, size_t len)
{
    if (!eng || !data)
        return -EINVAL;
    if (!eng->load_buf)
        return -EINVAL; /* no load in progress */

    if (len == 0 || offset != eng->load_got) {
        p4_engine_load_abort(eng);
        return -EINVAL;
    }

    /*
     * Compare against what is left rather than forming offset + len, which
     * would wrap.  offset is already known to equal load_got, and load_got
     * never exceeds load_total, so the subtraction cannot underflow.
     */
    if (len > eng->load_total - eng->load_got) {
        p4_engine_load_abort(eng);
        return -EINVAL;
    }

    memcpy(eng->load_buf + eng->load_got, data, len);
    eng->load_got += len;
    return 0;
}

int p4_engine_load_commit(struct p4_engine *eng)
{
    if (!eng)
        return -EINVAL;
    if (!eng->load_buf || eng->load_got != eng->load_total) {
        p4_engine_load_abort(eng);
        return -EINVAL;
    }

    struct p4_program *prog = NULL;
    int rc = p4_json_parse(eng->load_buf, eng->load_got, &prog, eng->load_err,
                           sizeof(eng->load_err));

    p4_engine_load_abort(eng);
    if (rc != 0)
        return rc;

    /* A program that replaces a running one only does so once it has been
     * accepted in full, so a bad load cannot leave the pipeline empty. */
    p4_json_free(eng->prog);
    eng->prog = prog;
    return 0;
}

void p4_engine_load_progress(const struct p4_engine *eng, size_t *got,
                             size_t *total)
{
    if (got)
        *got = eng ? eng->load_got : 0;
    if (total)
        *total = eng ? eng->load_total : 0;
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
