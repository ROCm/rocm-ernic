/*
 * Unit tests for the P4 match-action engine façade.
 *
 * The engine takes a frame and returns a verdict, with no device, no guest
 * memory and no socket behind it, so all of this runs under CTest.
 *
 * What is pinned down here is the contract the rest of the emulator depends
 * on: the port model, the bounds on frame size and output capacity, and the
 * no-program behaviour -- forward unchanged to the other port -- which is
 * what makes attaching the engine a no-op until a program is loaded.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "p4_engine.h"

static int failures;

static void check(bool cond, const char *what)
{
    if (!cond) {
        printf("p4_engine: FAIL %s\n", what);
        failures++;
    }
}

/* A frame that is distinguishable from an all-zero buffer. */
static void fill_frame(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(i * 7u + 1u);
}

static void test_lifecycle(void)
{
    struct p4_engine *eng = p4_engine_create();
    check(eng != NULL, "create returns an engine");
    check(!p4_engine_has_program(eng), "a fresh engine has no program");
    p4_engine_destroy(eng);

    /* Destroying NULL must be safe: the server's teardown path is shared
     * with the case where attach failed. */
    p4_engine_destroy(NULL);
    check(!p4_engine_has_program(NULL), "has_program(NULL) is false");
}

static void test_forward_unchanged(void)
{
    struct p4_engine *eng = p4_engine_create();
    uint8_t in[256];
    uint8_t out[sizeof(in)];
    struct p4_result res;

    fill_frame(in, sizeof(in));

    memset(out, 0, sizeof(out));
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            &res) == 0,
          "host-port frame is processed");
    check(res.verdict == P4_VERDICT_FORWARD, "host-port frame is forwarded");
    check(res.egress_port == P4_PORT_UPLINK, "host port egresses to uplink");
    check(res.len == sizeof(in), "length is preserved");
    check(memcmp(in, out, sizeof(in)) == 0, "bytes are preserved");

    memset(out, 0, sizeof(out));
    check(p4_engine_process(eng, P4_PORT_UPLINK, in, sizeof(in), out,
                            sizeof(out), &res) == 0,
          "uplink-port frame is processed");
    check(res.egress_port == P4_PORT_HOST, "uplink port egresses to host");
    check(memcmp(in, out, sizeof(in)) == 0, "bytes are preserved both ways");

    p4_engine_destroy(eng);
}

static void test_rejects_bad_input(void)
{
    struct p4_engine *eng = p4_engine_create();
    uint8_t in[64];
    uint8_t out[sizeof(in)];
    struct p4_result res;

    fill_frame(in, sizeof(in));

    check(p4_engine_process(NULL, P4_PORT_HOST, in, sizeof(in), out,
                            sizeof(out), &res) == -EINVAL,
          "NULL engine is rejected");
    check(p4_engine_process(eng, P4_PORT_HOST, NULL, sizeof(in), out,
                            sizeof(out), &res) == -EINVAL,
          "NULL frame is rejected");
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), NULL,
                            sizeof(out), &res) == -EINVAL,
          "NULL output buffer is rejected");
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            NULL) == -EINVAL,
          "NULL result is rejected");
    check(p4_engine_process(eng, P4_PORT_HOST, in, 0, out, sizeof(out), &res) ==
              -EINVAL,
          "a zero-length frame is rejected");

    /* An ingress port the device does not have must not index anything. */
    check(p4_engine_process(eng, P4_PORT_COUNT, in, sizeof(in), out,
                            sizeof(out), &res) == -EINVAL,
          "an out-of-range ingress port is rejected");
    check(p4_engine_process(eng, UINT32_MAX, in, sizeof(in), out, sizeof(out),
                            &res) == -EINVAL,
          "UINT32_MAX ingress port is rejected");

    /* An output buffer smaller than the frame must be refused outright
     * rather than truncating into it. */
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out,
                            sizeof(in) - 1, &res) == -ENOSPC,
          "a short output buffer is rejected");

    p4_engine_destroy(eng);
}

static void test_frame_size_limit(void)
{
    struct p4_engine *eng = p4_engine_create();
    static uint8_t in[P4_FRAME_MAX + 1];
    static uint8_t out[P4_FRAME_MAX + 1];
    struct p4_result res;
    struct p4_stats stats;

    fill_frame(in, sizeof(in));

    check(p4_engine_process(eng, P4_PORT_HOST, in, P4_FRAME_MAX, out,
                            sizeof(out), &res) == 0,
          "a frame at the limit is accepted");
    check(res.len == P4_FRAME_MAX, "the whole frame is carried");

    check(p4_engine_process(eng, P4_PORT_HOST, in, P4_FRAME_MAX + 1, out,
                            sizeof(out), &res) == -EMSGSIZE,
          "a frame over the limit is rejected");

    p4_engine_get_stats(eng, &stats);
    check(stats.frames_too_big == 1, "the oversized frame is counted");

    p4_engine_destroy(eng);
}

static void test_stats(void)
{
    struct p4_engine *eng = p4_engine_create();
    uint8_t in[128];
    uint8_t out[sizeof(in)];
    struct p4_result res;
    struct p4_stats stats;

    fill_frame(in, sizeof(in));

    p4_engine_get_stats(eng, &stats);
    check(stats.frames_in[P4_PORT_HOST] == 0, "stats start at zero");

    for (unsigned i = 0; i < 3; i++) {
        check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out,
                                sizeof(out), &res) == 0,
              "frame processed");
    }
    check(p4_engine_process(eng, P4_PORT_UPLINK, in, sizeof(in), out,
                            sizeof(out), &res) == 0,
          "frame processed");

    p4_engine_get_stats(eng, &stats);
    check(stats.frames_in[P4_PORT_HOST] == 3, "host ingress counted");
    check(stats.frames_in[P4_PORT_UPLINK] == 1, "uplink ingress counted");
    check(stats.frames_out[P4_PORT_UPLINK] == 3, "uplink egress counted");
    check(stats.frames_out[P4_PORT_HOST] == 1, "host egress counted");

    /* The getter must zero its output even when it has nothing to report,
     * because the caller may put the result on the wire. */
    memset(&stats, 0xAA, sizeof(stats));
    p4_engine_get_stats(NULL, &stats);
    check(stats.frames_in[P4_PORT_HOST] == 0,
          "NULL engine yields zeroed stats");
    p4_engine_get_stats(eng, NULL);

    p4_engine_destroy(eng);
}

/*
 * The program loader, driven the way a hostile guest would drive it. Every
 * one of these is a shape a transport can be made to produce, so each has
 * to fail cleanly rather than corrupt the accumulator or over-allocate.
 */
static void test_load_rejects_bad_input(void)
{
    struct p4_engine *eng = p4_engine_create();
    uint8_t data[64];
    size_t got, total;

    fill_frame(data, sizeof(data));

    check(p4_engine_load_begin(NULL, 64) == -EINVAL, "NULL engine is rejected");
    check(p4_engine_load_begin(eng, 0) == -EINVAL,
          "a zero-length program is rejected");
    check(p4_engine_load_begin(eng, P4_PROGRAM_MAX + 1) == -EINVAL,
          "a program over the cap is rejected");
    check(p4_engine_load_begin(eng, SIZE_MAX) == -EINVAL,
          "SIZE_MAX is rejected rather than allocated");

    /* A chunk with no load in progress must not touch anything. */
    check(p4_engine_load_chunk(eng, 0, data, sizeof(data)) == -EINVAL,
          "a chunk outside a load is rejected");
    check(p4_engine_load_commit(eng) == -EINVAL,
          "a commit outside a load is rejected");

    p4_engine_load_progress(eng, &got, &total);
    check(got == 0 && total == 0, "a rejected load left no state");

    p4_engine_destroy(eng);
}

static void test_load_chunk_bounds(void)
{
    struct p4_engine *eng = p4_engine_create();
    uint8_t data[64];
    size_t got, total;

    fill_frame(data, sizeof(data));

    /* A chunk running past the declared total must be refused, not
     * truncated into the buffer. */
    check(p4_engine_load_begin(eng, 32) == 0, "begin accepted");
    check(p4_engine_load_chunk(eng, 0, data, 64) == -EINVAL,
          "a chunk longer than the program is rejected");
    check(p4_engine_load_chunk(eng, 0, data, 32) == -EINVAL,
          "the load was abandoned after a bad chunk");

    /* An offset that is not where the loader is must be refused: accepting
     * it would leave a hole, and offset + len could wrap. */
    check(p4_engine_load_begin(eng, 64) == 0, "begin accepted");
    check(p4_engine_load_chunk(eng, 8, data, 8) == -EINVAL,
          "a chunk that skips forward is rejected");

    check(p4_engine_load_begin(eng, 64) == 0, "begin accepted");
    check(p4_engine_load_chunk(eng, 0, data, 32) == 0, "first chunk accepted");
    check(p4_engine_load_chunk(eng, 0, data, 32) == -EINVAL,
          "a chunk that rewrites what arrived is rejected");

    check(p4_engine_load_begin(eng, 64) == 0, "begin accepted");
    check(p4_engine_load_chunk(eng, UINT64_MAX, data, 8) == -EINVAL,
          "UINT64_MAX offset is rejected");

    check(p4_engine_load_begin(eng, 64) == 0, "begin accepted");
    check(p4_engine_load_chunk(eng, 0, data, 0) == -EINVAL,
          "a zero-length chunk is rejected");
    check(p4_engine_load_chunk(eng, 0, NULL, 8) == -EINVAL,
          "a NULL chunk is rejected");

    p4_engine_load_progress(eng, &got, &total);
    check(got == 0 && total == 0, "every rejection abandoned the load");

    p4_engine_destroy(eng);
}

static void test_load_sequence(void)
{
    struct p4_engine *eng = p4_engine_create();
    uint8_t data[96];
    size_t got, total;

    fill_frame(data, sizeof(data));

    check(p4_engine_load_begin(eng, sizeof(data)) == 0, "begin accepted");
    check(p4_engine_load_chunk(eng, 0, data, 32) == 0, "chunk 1 accepted");
    p4_engine_load_progress(eng, &got, &total);
    check(got == 32 && total == sizeof(data), "progress tracks the chunks");

    /* Committing early must fail rather than install a partial program. */
    check(p4_engine_load_commit(eng) == -EINVAL,
          "an incomplete program is not committed");
    check(!p4_engine_has_program(eng), "no program after a failed commit");

    check(p4_engine_load_begin(eng, sizeof(data)) == 0, "begin accepted");
    check(p4_engine_load_chunk(eng, 0, data, 32) == 0, "chunk 1 accepted");
    check(p4_engine_load_chunk(eng, 32, data + 32, 32) == 0,
          "chunk 2 accepted");
    check(p4_engine_load_chunk(eng, 64, data + 64, 32) == 0,
          "chunk 3 accepted");

    /* The loader is complete, so this is the parser's answer, not the
     * transport's: there is no parser yet. */
    check(p4_engine_load_commit(eng) == -ENOSYS,
          "a complete program reaches the parser");
    check(!p4_engine_has_program(eng),
          "no program is installed while the parser is absent");

    /* A begin abandons whatever was part-way through. */
    check(p4_engine_load_begin(eng, 64) == 0, "begin accepted");
    check(p4_engine_load_chunk(eng, 0, data, 16) == 0, "chunk accepted");
    check(p4_engine_load_begin(eng, 32) == 0, "a second begin is accepted");
    p4_engine_load_progress(eng, &got, &total);
    check(got == 0 && total == 32, "the second begin reset the accumulator");

    /* Destroying mid-load must not leak the accumulator. */
    p4_engine_destroy(eng);
}

static void test_load_whole_program_in_one_chunk(void)
{
    struct p4_engine *eng = p4_engine_create();
    static uint8_t big[64 * 1024];

    fill_frame(big, sizeof(big));
    check(p4_engine_load_begin(eng, sizeof(big)) == 0, "a large begin works");
    check(p4_engine_load_chunk(eng, 0, big, sizeof(big)) == 0,
          "a single chunk covering the program works");
    check(p4_engine_load_commit(eng) == -ENOSYS, "commit reaches the parser");

    p4_engine_load_abort(eng);
    p4_engine_load_abort(NULL);
    p4_engine_destroy(eng);
}

int main(void)
{
    test_lifecycle();
    test_load_rejects_bad_input();
    test_load_chunk_bounds();
    test_load_sequence();
    test_load_whole_program_in_one_chunk();
    test_forward_unchanged();
    test_rejects_bad_input();
    test_frame_size_limit();
    test_stats();

    if (failures) {
        printf("p4_engine: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("p4_engine: all checks passed\n");
    return 0;
}
