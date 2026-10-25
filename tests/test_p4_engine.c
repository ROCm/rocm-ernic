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
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p4_engine.h"
#include "p4_exec.h"
#include "p4_program.h"
#include "p4_fp8.h"
#include "p4_table.h"
#include "p4_value.h"

static int failures;

static void check(bool cond, const char *what)
{
    if (!cond) {
        printf("p4_engine: FAIL %s\n", what);
        failures++;
    }
}

/* For a failure whose reason the code under test produced. */
static void checkf(bool cond, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void checkf(bool cond, const char *fmt, ...)
{
    va_list ap;

    if (cond)
        return;
    printf("p4_engine: FAIL ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    failures++;
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

    /*
     * The NULL above is rejected by the no-load-in-progress check, since
     * the zero-length chunk already abandoned the load: it would pass
     * whether or not NULL abandons anything. This one starts a fresh
     * load so NULL is the first thing to fail, and then proves the load
     * really was abandoned rather than left open to retry into.
     */
    check(p4_engine_load_begin(eng, 64) == 0, "begin accepted");
    check(p4_engine_load_chunk(eng, 0, NULL, 8) == -EINVAL,
          "a NULL chunk is rejected with a load in progress");
    check(p4_engine_load_chunk(eng, 0, data, 32) == -EINVAL,
          "the load was abandoned after a NULL chunk");

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
     * transport's. These bytes are not JSON at all. */
    check(p4_engine_load_commit(eng) == -EINVAL,
          "a complete load reaches the parser, which refuses non-JSON");
    check(!p4_engine_has_program(eng), "no program after a refused parse");
    check(p4_engine_load_error(eng) != NULL, "the parser said why it refused");

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
    check(p4_engine_load_commit(eng) == -EINVAL, "commit reaches the parser");

    p4_engine_load_abort(eng);
    p4_engine_load_abort(NULL);
    p4_engine_destroy(eng);
}

/*
 * A header section, a parser over it, and a whole program, as text. Built
 * up in pieces so a case can introduce exactly one defect into one of them.
 */
/*
 * A header section carrying standard_metadata, which v1model always emits
 * and without which a program cannot say where a packet goes.
 */
#define P4_HDRS                                                \
    "{\"__meta__\":{\"version\":[2,24]},"                      \
    "\"header_types\":[{\"name\":\"eth_t\",\"id\":0,"          \
    "\"fields\":[[\"dst\",48],[\"src\",48],[\"ty\",16]]},"     \
    "{\"name\":\"std_t\",\"id\":1,"                            \
    "\"fields\":[[\"ingress_port\",9],[\"egress_spec\",9]]}]," \
    "\"headers\":[{\"name\":\"eth\",\"id\":0,"                 \
    "\"header_type\":\"eth_t\",\"metadata\":false},"           \
    "{\"name\":\"standard_metadata\",\"id\":1,"                \
    "\"header_type\":\"std_t\",\"metadata\":true}],"

#define P4_PARSER                                                    \
    P4_HDRS "\"parsers\":[{\"name\":\"p\",\"id\":0,"                 \
            "\"init_state\":\"start\",\"parse_states\":["            \
            "{\"name\":\"start\",\"id\":0,"                          \
            "\"parser_ops\":[{\"op\":\"extract\","                   \
            "\"parameters\":[{\"type\":\"regular\","                 \
            "\"value\":\"eth\"}]}],"                                 \
            "\"transition_key\":[],"                                 \
            "\"transitions\":[{\"value\":\"default\",\"mask\":null," \
            "\"next_state\":null}]}]}],"

#define P4_FRONT                                         \
    P4_PARSER "\"deparsers\":[{\"name\":\"d\",\"id\":0," \
              "\"order\":[\"eth\"]}],"

#define P4_SKELETON P4_FRONT "\"actions\":[]}"

/* Push a complete document through the engine's load path. */
static int load_text(struct p4_engine *eng, const char *json)
{
    size_t n = strlen(json);
    int rc = p4_engine_load_begin(eng, n);

    if (rc != 0)
        return rc;
    rc = p4_engine_load_chunk(eng, 0, json, n);
    if (rc != 0)
        return rc;
    return p4_engine_load_commit(eng);
}

/*
 * The loader, through the transport that will really carry a program. Only
 * the sections that are implemented are exercised here; the walker's own
 * test covers the rest.
 */
static void test_load_parses(void)
{
    struct p4_engine *eng = p4_engine_create();

    /* A bare value at the top level: json-c's STRICT flag accepts this, so
     * our own type check is what has to refuse it. */
    check(load_text(eng, "\"hello\"") == -EINVAL,
          "a bare string at the top level is refused");
    check(load_text(eng, "[1,2]") == -EINVAL,
          "a bare array at the top level is refused");
    check(load_text(eng, "{") == -EINVAL, "an unterminated object is refused");

    /* __meta__ gates the format. */
    check(load_text(eng, "{}") == -EINVAL, "a document with no __meta__");
    check(load_text(eng, "{\"__meta__\":{\"version\":[1,0]}}") == -ENOSYS,
          "an unreadable major version is reported as unsupported");
    check(load_text(eng, "{\"__meta__\":{\"version\":[2]}}") == -EINVAL,
          "a one-element version is refused");
    check(load_text(eng, "{\"__meta__\":{\"version\":[\"2\",0]}}") == -EINVAL,
          "a stringly-typed version is refused, not coerced");

    /* A feature we understand and have not built is named, not lumped in
     * with a malformed file. */
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},"
                         "\"checksums\":[{\"name\":\"x\"}]}") == -ENOSYS,
          "a non-empty checksums list is unsupported, not invalid");
    check(strstr(p4_engine_load_error(eng), "checksums") != NULL,
          "the error names the feature that was used");

    /*
     * A signed field is the same kind of refusal. Nothing carries
     * signedness: bits_get() zero-extends and value_cmp() is a memcmp,
     * so an int<8> of 0xff would compare greater than zero. Accepting
     * the flag and discarding it is the half-executed program the
     * loader exists to refuse. The unsigned form beside it must still
     * load, since p4c emits a third element of false routinely.
     */
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},"
                         "\"header_types\":[{\"name\":\"t\",\"id\":0,"
                         "\"fields\":[[\"a\",8,true]]}]}") == -ENOSYS,
          "a signed field is unsupported, not silently unsigned");
    check(strstr(p4_engine_load_error(eng), "signed") != NULL,
          "the error says the field was signed");
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},"
                         "\"header_types\":[{\"name\":\"t\",\"id\":0,"
                         "\"fields\":[[\"a\",8,false]]}]}") != -ENOSYS,
          "an explicitly unsigned field still loads");

    /* ...but mentioning it empty is fine, which is how p4c emits it for a
     * program that does not use one. The walk then stops at the first
     * section this document is missing. */
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},\"checksums\":[],"
                         "\"header_types\":[],\"headers\":[]}") == -EINVAL,
          "an empty checksums list is accepted");
    /* With no headers there is no standard_metadata, and a program that
     * cannot name egress_spec cannot say where a packet goes. */
    check(strstr(p4_engine_load_error(eng), "egress_spec") != NULL,
          "the walk got past the feature check, to the missing metadata");

    /* Header types and their fields, with a parser and deparser so the
     * walk reaches the part that is not written yet. */
    {
        check(load_text(eng, P4_SKELETON) == -EINVAL,
              "a program with no pipelines is refused");
        check(strstr(p4_engine_load_error(eng), "pipelines") != NULL,
              "the walk got past the parser, deparser and actions");
    }

    /* The parser. */
    check(load_text(eng, P4_HDRS "\"parsers\":[],\"deparsers\":[]}") == -ENOSYS,
          "a program with no parser is refused");
    check(load_text(eng, P4_HDRS "\"parsers\":[{\"name\":\"p\",\"id\":0,"
                                 "\"init_state\":\"nope\",\"parse_states\":["
                                 "{\"name\":\"start\",\"id\":0,"
                                 "\"parser_ops\":[],\"transition_key\":[],"
                                 "\"transitions\":[{\"value\":\"default\","
                                 "\"mask\":null,\"next_state\":null}]}]}],"
                                 "\"deparsers\":[]}") == -EINVAL,
          "an init_state that is not a parse state is refused");
    check(load_text(eng, P4_HDRS "\"parsers\":[{\"name\":\"p\",\"id\":0,"
                                 "\"init_state\":\"start\",\"parse_states\":["
                                 "{\"name\":\"start\",\"id\":0,"
                                 "\"parser_ops\":[],\"transition_key\":[],"
                                 "\"transitions\":[{\"value\":\"default\","
                                 "\"mask\":null,"
                                 "\"next_state\":\"nowhere\"}]}]}],"
                                 "\"deparsers\":[]}") == -EINVAL,
          "a transition to a state that does not exist is refused");
    check(load_text(eng, P4_HDRS "\"parsers\":[{\"name\":\"p\",\"id\":0,"
                                 "\"init_state\":\"start\",\"parse_states\":["
                                 "{\"name\":\"start\",\"id\":0,"
                                 "\"parser_ops\":[{\"op\":\"extract_VL\","
                                 "\"parameters\":[]}],"
                                 "\"transition_key\":[],"
                                 "\"transitions\":[]}]}],"
                                 "\"deparsers\":[]}") == -ENOSYS,
          "a parser operation we do not implement is named");
    check(strstr(p4_engine_load_error(eng), "extract_VL") != NULL,
          "the error names the parser operation");
    check(load_text(eng, P4_HDRS "\"parsers\":[{\"name\":\"p\",\"id\":0,"
                                 "\"init_state\":\"start\",\"parse_states\":["
                                 "{\"name\":\"start\",\"id\":0,"
                                 "\"parser_ops\":[{\"op\":\"extract\","
                                 "\"parameters\":[{\"type\":\"regular\","
                                 "\"value\":\"ghost\"}]}],"
                                 "\"transition_key\":[],"
                                 "\"transitions\":[]}]}],"
                                 "\"deparsers\":[]}") == -EINVAL,
          "extracting a header that does not exist is refused");

    /* Actions. */
#define P4_ACT(body) P4_FRONT "\"actions\":[" body "]}"

    /* The shapes p4c really emits, including both drop arities. */
    check(load_text(eng, P4_ACT("{\"name\":\"a\",\"id\":0,"
                                "\"runtime_data\":[{\"name\":\"p\","
                                "\"bitwidth\":48}],"
                                "\"primitives\":[{\"op\":\"modify_field\","
                                "\"parameters\":["
                                "{\"type\":\"field\","
                                "\"value\":[\"eth\",\"src\"]},"
                                "{\"type\":\"runtime_data\","
                                "\"value\":0}]}]}")) == -EINVAL,
          "modify_field from a runtime argument is walked");
    check(load_text(eng, P4_ACT("{\"name\":\"d\",\"id\":0,"
                                "\"runtime_data\":[],"
                                "\"primitives\":[{\"op\":\"drop\","
                                "\"parameters\":[]}]}")) == -EINVAL,
          "drop with no parameter is walked");
    check(load_text(eng, P4_ACT("{\"name\":\"d\",\"id\":0,"
                                "\"runtime_data\":[],"
                                "\"primitives\":[{\"op\":\"mark_to_drop\","
                                "\"parameters\":[{\"type\":\"header\","
                                "\"value\":\"standard_metadata\"}]}]}")) ==
              -EINVAL,
          "mark_to_drop with its metadata parameter is walked");

    /* Rejections. */
    /* A negative literal, which simple_router.json uses to decrement a
     * TTL. Run as its magnitude, it would make that TTL go up. */
    check(load_text(eng, P4_ACT("{\"name\":\"t\",\"id\":0,"
                                "\"runtime_data\":[],"
                                "\"primitives\":[{\"op\":\"add_to_field\","
                                "\"parameters\":["
                                "{\"type\":\"field\","
                                "\"value\":[\"eth\",\"ty\"]},"
                                "{\"type\":\"hexstr\","
                                "\"value\":\"-0x1\"}]}]}")) == -ENOSYS,
          "a negative literal is refused");
    check(strstr(p4_engine_load_error(eng), "negative") != NULL,
          "the error says the literal is negative");
    check(load_text(eng, P4_ACT("{\"name\":\"a\",\"id\":0,"
                                "\"runtime_data\":[],"
                                "\"primitives\":[{\"op\":\"push\","
                                "\"parameters\":[]}]}")) == -ENOSYS,
          "an unsupported primitive is refused");
    check(strstr(p4_engine_load_error(eng), "push") != NULL,
          "the error names the primitive");
    check(load_text(eng, P4_ACT("{\"name\":\"a\",\"id\":0,"
                                "\"runtime_data\":[],"
                                "\"primitives\":[{\"op\":\"modify_field\","
                                "\"parameters\":[{\"type\":\"field\","
                                "\"value\":[\"eth\",\"src\"]}]}]}")) == -EINVAL,
          "a primitive with too few parameters is refused");
    /* An index past the end would read an argument no entry supplies. */
    check(load_text(eng, P4_ACT("{\"name\":\"a\",\"id\":0,"
                                "\"runtime_data\":[{\"name\":\"p\","
                                "\"bitwidth\":8}],"
                                "\"primitives\":[{\"op\":\"modify_field\","
                                "\"parameters\":["
                                "{\"type\":\"field\","
                                "\"value\":[\"eth\",\"src\"]},"
                                "{\"type\":\"runtime_data\","
                                "\"value\":1}]}]}")) == -EINVAL,
          "a runtime argument index past the end is refused");
    check(load_text(eng, P4_ACT("{\"name\":\"a\",\"id\":0,"
                                "\"runtime_data\":[],"
                                "\"primitives\":[{\"op\":\"modify_field\","
                                "\"parameters\":["
                                "{\"type\":\"field\","
                                "\"value\":[\"eth\",\"ghost\"]},"
                                "{\"type\":\"hexstr\","
                                "\"value\":\"0x1\"}]}]}")) == -EINVAL,
          "writing to a field that does not exist is refused");
    /* An expression operand needs machinery this does not have. */
    check(load_text(eng, P4_ACT("{\"name\":\"a\",\"id\":0,"
                                "\"runtime_data\":[],"
                                "\"primitives\":[{\"op\":\"modify_field\","
                                "\"parameters\":["
                                "{\"type\":\"field\","
                                "\"value\":[\"eth\",\"src\"]},"
                                "{\"type\":\"register\","
                                "\"value\":[\"r\",0]}]}]}")) == -ENOSYS,
          "a register operand is refused by name");
    /* Assigning into a literal is not a place a value can go. */
    check(load_text(eng, P4_ACT("{\"name\":\"a\",\"id\":0,"
                                "\"runtime_data\":[],"
                                "\"primitives\":[{\"op\":\"modify_field\","
                                "\"parameters\":["
                                "{\"type\":\"hexstr\",\"value\":\"0x1\"},"
                                "{\"type\":\"hexstr\","
                                "\"value\":\"0x2\"}]}]}")) == -EINVAL,
          "assigning to a literal is refused");
    check(load_text(eng, P4_ACT("{\"name\":\"a\",\"id\":0,"
                                "\"runtime_data\":[],"
                                "\"primitives\":[]},"
                                "{\"name\":\"a\",\"id\":1,"
                                "\"runtime_data\":[],"
                                "\"primitives\":[]}")) == -EINVAL,
          "two actions with one name are refused");

    /* The deparser. */
    check(load_text(eng, P4_PARSER "\"deparsers\":[{\"name\":\"d\","
                                   "\"id\":0,"
                                   "\"order\":[\"ghost\"]}]}") == -EINVAL,
          "a deparser emitting a header that does not exist is refused");
    check(load_text(eng, P4_PARSER "\"deparsers\":[{\"name\":\"d\","
                                   "\"id\":0,"
                                   "\"order\":[\"eth\",\"eth\"]}]}") == -EINVAL,
          "a deparser emitting one header twice is refused");

    /* A variable-width field is named rather than called malformed. */
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},"
                         "\"header_types\":[{\"name\":\"t\",\"id\":0,"
                         "\"fields\":[[\"opts\",\"*\"]]}]}") == -ENOSYS,
          "a variable-length field is unsupported, not invalid");
    check(strstr(p4_engine_load_error(eng), "variable length") != NULL,
          "the error says the field is variable length");

    /* Integer edges on a field width. */
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},"
                         "\"header_types\":[{\"name\":\"t\",\"id\":0,"
                         "\"fields\":[[\"f\",0]]}]}") == -EINVAL,
          "a zero-width field is refused");
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},"
                         "\"header_types\":[{\"name\":\"t\",\"id\":0,"
                         "\"fields\":[[\"f\",513]]}]}") == -EINVAL,
          "an over-wide field is refused");
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},"
                         "\"header_types\":[{\"name\":\"t\",\"id\":0,"
                         "\"fields\":[[\"f\",1.0]]}]}") == -EINVAL,
          "a field width given as a double is refused, not rounded");

    /* Duplicates make every later reference ambiguous. */
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},"
                         "\"header_types\":[{\"name\":\"t\",\"id\":0,"
                         "\"fields\":[[\"f\",8]]},"
                         "{\"name\":\"t\",\"id\":1,"
                         "\"fields\":[[\"f\",8]]}]}") == -EINVAL,
          "two header types with one name are refused");
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},"
                         "\"header_types\":[{\"name\":\"a\",\"id\":7,"
                         "\"fields\":[[\"f\",8]]},"
                         "{\"name\":\"b\",\"id\":7,"
                         "\"fields\":[[\"f\",8]]}]}") == -EINVAL,
          "two header types with one id are refused");

    /* A header naming a type that does not exist. */
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},"
                         "\"header_types\":[],"
                         "\"headers\":[{\"name\":\"h\",\"id\":0,"
                         "\"header_type\":\"nope\","
                         "\"metadata\":false}]}") == -EINVAL,
          "a dangling header_type reference is refused");

    /* A name that would inject into the log it is reported in. */
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},"
                         "\"header_types\":[{\"name\":\"a\\nb\",\"id\":0,"
                         "\"fields\":[[\"f\",8]]}]}") == -EINVAL,
          "a name containing a newline is refused");

    /* Pipelines, tables and conditionals. */
#define P4_PIPE(body)                                   \
    P4_FRONT "\"actions\":[{\"name\":\"a\",\"id\":0,"   \
             "\"runtime_data\":[],\"primitives\":[]}]," \
             "\"pipelines\":[" body "]}"

#define P4_TBL(name, nextbody)                                  \
    "{\"name\":\"" name "\",\"id\":0,\"match_type\":\"exact\"," \
    "\"type\":\"simple\",\"max_size\":16,"                      \
    "\"with_counters\":false,\"direct_meters\":null,"           \
    "\"support_timeout\":false,"                                \
    "\"key\":[{\"match_type\":\"exact\","                       \
    "\"target\":[\"eth\",\"dst\"],\"mask\":null}],"             \
    "\"actions\":[\"a\"],\"next_tables\":" nextbody "}"

    check(load_text(eng,
                    P4_PIPE("{\"name\":\"ingress\",\"id\":0,"
                            "\"init_table\":\"t\",\"tables\":[" P4_TBL(
                                "t", "{\"a\":null}") "],\"action_profiles\":[],"
                                                     "\"conditionals\":[]}")) ==
              0,
          "a one-table pipeline loads");

    /* A table that leads back to itself is an infinite loop on the thread
     * that also services the vfio-user poll loop. */
    check(load_text(
              eng, P4_PIPE("{\"name\":\"ingress\",\"id\":0,"
                           "\"init_table\":\"t\",\"tables\":[" P4_TBL(
                               "t", "{\"a\":\"t\"}") "],\"action_profiles\":[],"
                                                     "\"conditionals\":[]}")) ==
              -EINVAL,
          "a self-looping table is refused");
    check(strstr(p4_engine_load_error(eng), "loops back") != NULL,
          "the error says the pipeline loops");

    /* A two-table cycle, which a single-node check would miss. */
    check(
        load_text(
            eng,
            P4_PIPE(
                "{\"name\":\"ingress\",\"id\":0,"
                "\"init_table\":\"t\",\"tables\":[" P4_TBL(
                    "t",
                    "{\"a\":\"u\"}") "," P4_TBL("u",
                                                "{\"a\":\"t\"}") "],\"action_"
                                                                 "profiles\":[]"
                                                                 ","
                                                                 "\"conditional"
                                                                 "s\":[]}")) ==
            -EINVAL,
        "a two-table cycle is refused");

    /* A cycle reachable only through the miss path. */
    check(load_text(eng, P4_PIPE("{\"name\":\"ingress\",\"id\":0,"
                                 "\"init_table\":\"t\",\"tables\":["
                                 "{\"name\":\"t\",\"id\":0,"
                                 "\"match_type\":\"exact\","
                                 "\"type\":\"simple\",\"max_size\":16,"
                                 "\"with_counters\":false,"
                                 "\"direct_meters\":null,"
                                 "\"support_timeout\":false,"
                                 "\"key\":[],\"actions\":[\"a\"],"
                                 "\"next_tables\":{\"a\":null},"
                                 "\"base_default_next\":\"t\"}"
                                 "],\"action_profiles\":[],"
                                 "\"conditionals\":[]}")) == -EINVAL,
          "a cycle through base_default_next is refused");

    /* next_tables may key by action name or by hit/miss, never both. */
    check(load_text(
              eng,
              P4_PIPE("{\"name\":\"ingress\",\"id\":0,"
                      "\"init_table\":\"t\",\"tables\":[" P4_TBL(
                          "t", "{\"a\":null,"
                               "\"__HIT__\":null}") "],\"action_profiles\":[],"
                                                    "\"conditionals\":[]}")) ==
              -EINVAL,
          "next_tables mixing __HIT__ with an action name is refused");

    /* A next_tables key that is not one of the table's actions would
     * never be consulted. */
    check(load_text(
              eng,
              P4_PIPE(
                  "{\"name\":\"ingress\",\"id\":0,"
                  "\"init_table\":\"t\",\"tables\":[" P4_TBL(
                      "t",
                      "{\"a\":null,\"ghost\":null}") "],\"action_profiles\":[],"
                                                     "\"conditionals\":[]}")) ==
              -EINVAL,
          "a next_tables key that is not an action is refused");

    /* const entries make a table immutable and size-zero in bmv2, so an
     * empty array is not the harmless default it looks like. */
    check(load_text(eng, P4_PIPE("{\"name\":\"ingress\",\"id\":0,"
                                 "\"init_table\":\"t\",\"tables\":["
                                 "{\"name\":\"t\",\"id\":0,"
                                 "\"match_type\":\"exact\","
                                 "\"type\":\"simple\",\"max_size\":16,"
                                 "\"with_counters\":false,"
                                 "\"direct_meters\":null,"
                                 "\"support_timeout\":false,\"entries\":[],"
                                 "\"key\":[],\"actions\":[\"a\"],"
                                 "\"next_tables\":{\"a\":null}}"
                                 "],\"action_profiles\":[],"
                                 "\"conditionals\":[]}")) == -ENOSYS,
          "a table with const entries is refused, even an empty list");

    /* The aggregate entry budget: max_size is the one number that names a
     * large allocation without the document's size bounding it. */
    check(load_text(eng, P4_PIPE("{\"name\":\"ingress\",\"id\":0,"
                                 "\"init_table\":\"t\",\"tables\":["
                                 "{\"name\":\"t\",\"id\":0,"
                                 "\"match_type\":\"exact\","
                                 "\"type\":\"simple\","
                                 "\"max_size\":65535,"
                                 "\"with_counters\":false,"
                                 "\"direct_meters\":null,"
                                 "\"support_timeout\":false,"
                                 "\"key\":[],\"actions\":[\"a\"],"
                                 "\"next_tables\":{\"a\":null}},"
                                 "{\"name\":\"u\",\"id\":1,"
                                 "\"match_type\":\"exact\","
                                 "\"type\":\"simple\","
                                 "\"max_size\":65535,"
                                 "\"with_counters\":false,"
                                 "\"direct_meters\":null,"
                                 "\"support_timeout\":false,"
                                 "\"key\":[],\"actions\":[\"a\"],"
                                 "\"next_tables\":{\"a\":null}}"
                                 "],\"action_profiles\":[],"
                                 "\"conditionals\":[]}")) == -EINVAL,
          "two tables that together exceed the entry budget are refused");

    /* A pipeline must have an ingress. */
    check(load_text(eng, P4_PIPE("{\"name\":\"egress\",\"id\":0,"
                                 "\"init_table\":null,\"tables\":[],"
                                 "\"action_profiles\":[],"
                                 "\"conditionals\":[]}")) == -EINVAL,
          "a program with no ingress pipeline is refused");

    p4_engine_destroy(eng);
}

/*
 * The loader against real compiler output. This is the regression test for
 * two findings that a hand-written fixture would never have caught: p4c
 * emits keys the format documentation does not mention, and it emits a
 * non-empty 'errors' table into every artifact. A loader strict about
 * either refuses everything p4c produces.
 */
static char *read_fixture(const char *name, size_t *len)
{
    char path[512];
    FILE *f;
    char *buf;
    long n;

    (void)snprintf(path, sizeof(path), "%s/%s", P4_FIXTURE_DIR, name);
    f = fopen(path, "rbe"); /* e: O_CLOEXEC */
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int load_fixture(struct p4_engine *eng, const char *name)
{
    size_t n = 0;
    char *buf = read_fixture(name, &n);
    int rc;

    if (!buf)
        return -ENOENT;
    rc = p4_engine_load_begin(eng, n);
    if (rc == 0)
        rc = p4_engine_load_chunk(eng, 0, buf, n);
    if (rc == 0)
        rc = p4_engine_load_commit(eng);
    free(buf);
    return rc;
}

static void test_load_real_artifacts(void)
{
    struct p4_engine *eng = p4_engine_create();
    const char *why;

    /*
     * loopback.json carries program, force_arith, source_info and pi_omit,
     * none of which the format documentation mentions, and six entries in
     * 'errors'. It must get all the way to the unfinished part of the walk.
     */
    check(load_fixture(eng, "loopback.json") == 0,
          "bmv2's own loopback.json loads");
    check(p4_engine_has_program(eng), "a program is installed");

    /*
     * simple_router.json is the negative fixture: its calculations and
     * checksums carry the IPv4 checksum, which is not implemented. It
     * becomes a positive fixture when checksums land.
     */
    check(load_fixture(eng, "simple_router.json") == -ENOSYS,
          "simple_router.json is refused");
    why = p4_engine_load_error(eng);
    check(why != NULL && (strstr(why, "checksums") != NULL ||
                          strstr(why, "calculations") != NULL),
          "simple_router.json is refused by name, for the checksum");

    /*
     * The hand-written fixture: a program small enough to reason about
     * that is nonetheless meant to forward a frame. It must walk as far as
     * loopback.json does, which is what proves the subset is expressible
     * rather than merely tolerant of what p4c happens to emit.
     */
    check(load_fixture(eng, "minimal_forward.json") == 0,
          "the minimal forwarding program loads");
    check(p4_engine_has_program(eng), "a program is installed");

    p4_engine_destroy(eng);
}

/*
 * Running a loaded program over a frame. This is what the whole loader is
 * for: the decision has to come from the program, not from the engine.
 */
static void test_execute(void)
{
    struct p4_engine *eng = p4_engine_create();
    uint8_t in[64], out[128];
    struct p4_result res;
    struct p4_stats st;

    for (size_t i = 0; i < sizeof(in); i++)
        in[i] = (uint8_t)i;

    /* bmv2's own loopback program forwards what it is given, unchanged. */
    check(load_fixture(eng, "loopback.json") == 0, "loopback.json loads");
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            &res) == 0,
          "a frame runs through the program");
    check(res.verdict == P4_VERDICT_FORWARD, "loopback forwards the frame");
    check(res.len == sizeof(in) && memcmp(in, out, sizeof(in)) == 0,
          "the frame comes out byte for byte");

    /*
     * The minimal program's table has no entries and its default action is
     * a drop, so every packet misses and is dropped. The program decides,
     * not the engine.
     */
    check(load_fixture(eng, "minimal_forward.json") == 0,
          "minimal_forward.json loads");
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            &res) == 0,
          "a frame runs through the program");
    check(res.verdict == P4_VERDICT_DROP,
          "a table whose default action drops, drops");

    p4_engine_get_stats(eng, &st);
    check(st.frames_dropped == 1, "the drop is counted");

    p4_engine_destroy(eng);
}

/* A program whose default action sends every frame to a chosen port. */
#define P4_TO_PORT(port)                                              \
    P4_PARSER "\"deparsers\":[{\"name\":\"d\",\"id\":0,"              \
              "\"order\":[\"eth\"]}],"                                \
              "\"actions\":[{\"name\":\"fwd\",\"id\":0,"              \
              "\"runtime_data\":[],"                                  \
              "\"primitives\":[{\"op\":\"assign\",\"parameters\":["   \
              "{\"type\":\"field\",\"value\":[\"standard_metadata\"," \
              "\"egress_spec\"]},"                                    \
              "{\"type\":\"hexstr\",\"value\":\"" port "\"}]}]}],"    \
              "\"pipelines\":[{\"name\":\"ingress\",\"id\":0,"        \
              "\"init_table\":\"t\",\"tables\":["                     \
              "{\"name\":\"t\",\"id\":0,\"match_type\":\"exact\","    \
              "\"type\":\"simple\",\"max_size\":16,"                  \
              "\"with_counters\":false,\"direct_meters\":null,"       \
              "\"support_timeout\":false,\"key\":[],"                 \
              "\"actions\":[\"fwd\"],"                                \
              "\"next_tables\":{\"fwd\":null},"                       \
              "\"default_entry\":{\"action_id\":0,"                   \
              "\"action_data\":[]}}],"                                \
              "\"action_profiles\":[],\"conditionals\":[]}]}"

/*
 * A program that runs without assigning egress_spec has not chosen a
 * port, and the frame goes to the other port -- what the device does with
 * no program at all.
 *
 * fp8_policy.p4 is exactly such a program: it governs the RDMA path and
 * its ingress never touches egress_spec.  The field's initial zero used
 * to be read as P4_PORT_HOST, so a guest that loaded it had every frame
 * it transmitted injected back into its own receive ring -- a silent
 * loopback, counted as forwarded rather than dropped.
 */
static void test_execute_no_egress_assignment(void)
{
    struct p4_engine *eng = p4_engine_create();
    uint8_t in[64], out[128];
    struct p4_result res;
    struct p4_stats st;

    for (size_t i = 0; i < sizeof(in); i++)
        in[i] = (uint8_t)i;

    check(load_fixture(eng, "fp8_policy.json") == 0, "fp8_policy.json loads");

    /* Host ingress must leave by the uplink, not come back to the host. */
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            &res) == 0,
          "a frame runs through the RDMA-only program");
    check(res.verdict == P4_VERDICT_FORWARD,
          "a frame under a program that assigns no egress_spec is forwarded");
    check(res.egress_port == P4_PORT_UPLINK,
          "an unassigned egress_spec is not read as port 0");

    /* And the other direction, for the same reason. */
    check(p4_engine_process(eng, P4_PORT_UPLINK, in, sizeof(in), out,
                            sizeof(out), &res) == 0,
          "an uplink frame runs through it too");
    check(res.verdict == P4_VERDICT_FORWARD && res.egress_port == P4_PORT_HOST,
          "an uplink frame goes to the host");

    p4_engine_get_stats(eng, &st);
    check(st.frames_dropped == 0, "neither frame was dropped");
    check(st.frames_out[P4_PORT_UPLINK] == 1 &&
              st.frames_out[P4_PORT_HOST] == 1,
          "each frame was counted against the port it left by");

    p4_engine_destroy(eng);
}

static void test_execute_egress(void)
{
    struct p4_engine *eng = p4_engine_create();
    uint8_t in[64], out[128];
    struct p4_result res;

    for (size_t i = 0; i < sizeof(in); i++)
        in[i] = (uint8_t)i;

    /* To the uplink: the ordinary direction. */
    check(load_text(eng, P4_TO_PORT("0x1")) == 0, "a forwarding program "
                                                  "loads");
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            &res) == 0,
          "the frame runs");
    check(res.verdict == P4_VERDICT_FORWARD &&
              res.egress_port == P4_PORT_UPLINK,
          "the program sent the frame to the uplink");

    /*
     * Back to the port it came from. This is the loop that was
     * unreachable until a program could set egress_spec, and it is what
     * lets a guest frame be answered without a TAP.
     */
    check(load_text(eng, P4_TO_PORT("0x0")) == 0, "a looping program loads");
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            &res) == 0,
          "the frame runs");
    check(res.verdict == P4_VERDICT_FORWARD && res.egress_port == P4_PORT_HOST,
          "the program sent the frame back to the host port");

    /* A port this device does not have is a drop, never an index. */
    check(load_text(eng, P4_TO_PORT("0x9")) == 0, "a program naming port 9 "
                                                  "loads");
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            &res) == 0,
          "the frame runs");
    check(res.verdict == P4_VERDICT_DROP,
          "an egress_spec outside this device's ports is dropped");

    /* A frame too short for the headers the parser wants. */
    check(load_text(eng, P4_TO_PORT("0x1")) == 0, "reload");
    check(p4_engine_process(eng, P4_PORT_HOST, in, 4, out, sizeof(out), &res) ==
              0,
          "a short frame runs");
    check(res.verdict == P4_VERDICT_DROP, "a frame too short to parse is "
                                          "dropped");

    p4_engine_destroy(eng);
}

/*
 * Fields wider than a machine word. The program sets a 128-bit field to
 * 2^64 -- zero in every bit below 64 -- and tests it against zero. An
 * executor that read only the low 64 bits would see zero, take the true
 * branch and drop, so the two branches tell the implementations apart.
 */
static void test_execute_wide_field(void)
{
    struct p4_engine *eng = p4_engine_create();
    uint8_t in[64], out[128];
    struct p4_result res;

    for (size_t i = 0; i < sizeof(in); i++)
        in[i] = (uint8_t)i;

    check(load_fixture(eng, "wide_field.json") == 0,
          "the 128-bit field program loads");
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            &res) == 0,
          "the frame runs");
    check(res.verdict == P4_VERDICT_FORWARD,
          "2^64 is not zero, so the frame is forwarded");
    check(res.egress_port == P4_PORT_UPLINK,
          "the false branch chose the uplink");

    p4_engine_destroy(eng);
}

/* Fill in a match spec from a big-endian value, right-aligned. */
static void set_spec(struct p4_match_spec *m, const uint8_t *v, uint16_t len)
{
    memset(m, 0, sizeof(*m));
    m->len = len;
    memcpy(m->value + (P4_MATCH_BYTES - len), v, len);
}

/*
 * Table entries. Until now every lookup missed, so a table could only ever
 * do one thing. This is what makes the engine programmable rather than
 * merely loadable: the same program, the same frame, a different answer
 * because of an entry.
 */
static void test_table_entries(void)
{
    struct p4_engine *eng = p4_engine_create();
    uint8_t in[64], out[128];
    struct p4_result res;
    struct p4_update up;
    char err[160];

    for (size_t i = 0; i < sizeof(in); i++)
        in[i] = (uint8_t)i;

    /* minimal_forward's table has one exact key on ethernet.dstAddr and a
     * default action that drops. */
    check(load_fixture(eng, "minimal_forward.json") == 0, "the program loads");
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            &res) == 0,
          "a frame runs");
    check(res.verdict == P4_VERDICT_DROP,
          "with no entries the default action drops");

    /* An entry for this frame's destination MAC, sending it to port 1. */
    memset(&up, 0, sizeof(up));
    up.type = P4_UPDATE_INSERT;
    /* p4c qualifies every name with its control, so real compiler
     * output never carries a bare "forward". */
    (void)snprintf(up.table, sizeof(up.table), "IngressImpl.forward");
    (void)snprintf(up.action, sizeof(up.action), "IngressImpl.set_egress");
    up.nmatch = 1;
    set_spec(&up.match[0], in, 6); /* the first six bytes are dstAddr */
    up.nparams = 1;
    up.params[0].len = 1;
    up.params[0].value[P4_MATCH_BYTES - 1] = 1;

    checkf(p4_engine_update(eng, &up, err, sizeof(err)) == 0,
           "the entry is not installed: %s", err);
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            &res) == 0,
          "the same frame runs again");
    check(res.verdict == P4_VERDICT_FORWARD &&
              res.egress_port == P4_PORT_UPLINK,
          "the entry forwarded a frame the default action would have "
          "dropped");

    /* A frame whose destination is not in the table still misses. */
    {
        uint8_t other[64];

        memcpy(other, in, sizeof(other));
        other[0] ^= 0xFFu;
        check(p4_engine_process(eng, P4_PORT_HOST, other, sizeof(other), out,
                                sizeof(out), &res) == 0,
              "another frame runs");
        check(res.verdict == P4_VERDICT_DROP,
              "a destination with no entry still takes the default action");
    }

    /* Inserting the same key twice is a conflict, not a silent overwrite. */
    check(p4_engine_update(eng, &up, err, sizeof(err)) == -EEXIST,
          "a duplicate insert is refused");

    /* Modify changes where it goes. */
    up.type = P4_UPDATE_MODIFY;
    up.params[0].value[P4_MATCH_BYTES - 1] = 0;
    checkf(p4_engine_update(eng, &up, err, sizeof(err)) == 0,
           "the entry is not modified: %s", err);
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            &res) == 0,
          "the frame runs again");
    check(res.verdict == P4_VERDICT_FORWARD && res.egress_port == P4_PORT_HOST,
          "the modified entry sends it back to the host port");

    /* Delete restores the default. */
    up.type = P4_UPDATE_DELETE;
    checkf(p4_engine_update(eng, &up, err, sizeof(err)) == 0,
           "the entry is not deleted: %s", err);
    check(p4_engine_update(eng, &up, err, sizeof(err)) == -ENOENT,
          "deleting it twice is refused");
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            &res) == 0,
          "the frame runs once more");
    check(res.verdict == P4_VERDICT_DROP,
          "with the entry gone the default action drops again");

    p4_engine_destroy(eng);
}

static void test_table_entry_errors(void)
{
    struct p4_engine *eng = p4_engine_create();
    struct p4_update up;
    char err[160];

    memset(&up, 0, sizeof(up));
    up.type = P4_UPDATE_INSERT;
    /* p4c qualifies every name with its control, so real compiler
     * output never carries a bare "forward". */
    (void)snprintf(up.table, sizeof(up.table), "IngressImpl.forward");
    (void)snprintf(up.action, sizeof(up.action), "IngressImpl.set_egress");
    up.nmatch = 1;
    up.nparams = 1;
    up.params[0].len = 1;

    /* Nothing can be installed before a program says what a table is. */
    check(p4_engine_update(eng, &up, err, sizeof(err)) == -ENOENT,
          "an update with no program loaded is refused");

    check(load_fixture(eng, "minimal_forward.json") == 0, "the program loads");

    (void)snprintf(up.table, sizeof(up.table), "ghost");
    check(p4_engine_update(eng, &up, err, sizeof(err)) == -EINVAL,
          "an entry for a table that does not exist is refused");
    check(strstr(err, "ghost") != NULL, "the error names the table");

    /* p4c qualifies every name with its control, so real compiler
     * output never carries a bare "forward". */
    (void)snprintf(up.table, sizeof(up.table), "IngressImpl.forward");
    (void)snprintf(up.action, sizeof(up.action), "ghost");
    check(p4_engine_update(eng, &up, err, sizeof(err)) == -EINVAL,
          "an entry naming an action the table cannot run is refused");

    (void)snprintf(up.action, sizeof(up.action), "IngressImpl.set_egress");
    up.nmatch = 0;
    check(p4_engine_update(eng, &up, err, sizeof(err)) == -EINVAL,
          "an entry with the wrong number of key fields is refused");

    up.nmatch = 1;
    up.nparams = 0;
    check(p4_engine_update(eng, &up, err, sizeof(err)) == -EINVAL,
          "an entry with the wrong number of arguments is refused");

    /* A key wider than the field it matches would be truncated on use,
     * which is a different entry from the one asked for. */
    up.nparams = 1;
    up.match[0].len = P4_MATCH_BYTES;
    check(p4_engine_update(eng, &up, err, sizeof(err)) == -EINVAL,
          "a key field wider than the field it matches is refused");

    /* Likewise an argument wider than its parameter. */
    up.match[0].len = 6;
    up.params[0].len = P4_MATCH_BYTES;
    check(p4_engine_update(eng, &up, err, sizeof(err)) == -EINVAL,
          "an argument wider than its parameter is refused");

    p4_engine_destroy(eng);
}

/*
 * A program that matches on an RDMA operation rather than a frame. It
 * declares rdma_metadata and no parser operations, so it never touches a
 * packet.
 */
#define P4_RDMA_HDRS                                                    \
    "{\"__meta__\":{\"version\":[2,24]},"                               \
    "\"header_types\":[{\"name\":\"std_t\",\"id\":0,"                   \
    "\"fields\":[[\"ingress_port\",9],[\"egress_spec\",9]]},"           \
    "{\"name\":\"rdma_t\",\"id\":1,"                                    \
    "\"fields\":[[\"opcode\",8],[\"qp_num\",24],[\"length\",32],"       \
    "[\"imm_data\",32],[\"rkey\",32]]}],"                               \
    "\"headers\":[{\"name\":\"standard_metadata\",\"id\":0,"            \
    "\"header_type\":\"std_t\",\"metadata\":true},"                     \
    "{\"name\":\"rdma_metadata\",\"id\":1,"                             \
    "\"header_type\":\"rdma_t\",\"metadata\":true}],"                   \
    "\"parsers\":[{\"name\":\"p\",\"id\":0,\"init_state\":\"start\","   \
    "\"parse_states\":[{\"name\":\"start\",\"id\":0,\"parser_ops\":[]," \
    "\"transition_key\":[],\"transitions\":[{\"value\":\"default\","    \
    "\"mask\":null,\"next_state\":null}]}]}],"                          \
    "\"deparsers\":[{\"name\":\"d\",\"id\":0,\"order\":[]}],"

/* One table keyed on the operation's opcode, whose action drops. */
#define P4_RDMA_PROG                                                  \
    P4_RDMA_HDRS "\"actions\":[{\"name\":\"nope\",\"id\":0,"          \
                 "\"runtime_data\":[],"                               \
                 "\"primitives\":[{\"op\":\"drop\","                  \
                 "\"parameters\":[]}]}],"                             \
                 "\"pipelines\":[{\"name\":\"ingress\",\"id\":0,"     \
                 "\"init_table\":\"t\",\"tables\":["                  \
                 "{\"name\":\"t\",\"id\":0,\"match_type\":\"exact\"," \
                 "\"type\":\"simple\",\"max_size\":16,"               \
                 "\"with_counters\":false,\"direct_meters\":null,"    \
                 "\"support_timeout\":false,"                         \
                 "\"key\":[{\"match_type\":\"exact\","                \
                 "\"target\":[\"rdma_metadata\",\"opcode\"],"         \
                 "\"mask\":null}],"                                   \
                 "\"actions\":[\"nope\"],"                            \
                 "\"next_tables\":{\"nope\":null}}],"                 \
                 "\"action_profiles\":[],\"conditionals\":[]}]}"

static void test_rdma_metadata(void)
{
    struct p4_engine *eng = p4_engine_create();
    char err[160];

    /* An Ethernet-only program declares no rdma_metadata, and is simply
     * never run on that path. */
    check(load_fixture(eng, "loopback.json") == 0, "an Ethernet program "
                                                   "loads");

    /*
     * Declared at the wrong width, the field would resolve on its name
     * and then silently carry only part of every opcode. The program is
     * otherwise complete, because the architecture fields are resolved
     * after the deparser and a program that stops earlier never reaches
     * the check.
     */
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},"
                         "\"header_types\":[{\"name\":\"std_t\",\"id\":0,"
                         "\"fields\":[[\"ingress_port\",9],"
                         "[\"egress_spec\",9]]},"
                         "{\"name\":\"rdma_t\",\"id\":1,"
                         "\"fields\":[[\"opcode\",4]]}],"
                         "\"headers\":[{\"name\":\"standard_metadata\","
                         "\"id\":0,\"header_type\":\"std_t\","
                         "\"metadata\":true},"
                         "{\"name\":\"rdma_metadata\",\"id\":1,"
                         "\"header_type\":\"rdma_t\",\"metadata\":true}],"
                         "\"parsers\":[{\"name\":\"p\",\"id\":0,"
                         "\"init_state\":\"start\",\"parse_states\":["
                         "{\"name\":\"start\",\"id\":0,\"parser_ops\":[],"
                         "\"transition_key\":[],"
                         "\"transitions\":[{\"value\":\"default\","
                         "\"mask\":null,\"next_state\":null}]}]}],"
                         "\"deparsers\":[{\"name\":\"d\",\"id\":0,"
                         "\"order\":[]}],\"actions\":[]}") == -EINVAL,
          "an architecture field at the wrong width is refused");
    check(strstr(p4_engine_load_error(eng), "must be 8") != NULL,
          "the error says what the width must be");

    /*
     * Architecture state must not reach a wire. The check is on the
     * deparser rather than on the metadata flag, because p4c marks a
     * header-typed field of the metadata struct metadata:false -- it has
     * a validity bit -- so a program written the ordinary way would
     * otherwise be refused for a reason that is not the real one.
     */
    check(load_fixture(eng, "fp8_policy.json") == 0,
          "a real compiled policy program loads");

    (void)err;
    p4_engine_destroy(eng);
}

/*
 * Running the pipeline over an operation. The trap guarded here is the
 * one that would have bricked the device: a table with no entries runs no
 * action, egress_spec stays zero, and zero is a real port -- so a verdict
 * keyed on egress_spec would suppress every operation on a device whose
 * tables are empty, which is every device until a control plane runs.
 */
static void test_rdma_execute(void)
{
    struct p4_engine *eng = p4_engine_create();
    struct p4_rdma_meta meta;
    struct p4_update up;
    bool suppress = false;
    char err[160];

    memset(&meta, 0, sizeof(meta));
    meta.opcode = 4; /* an RDMA WRITE, say */
    meta.length = 4096;

    check(load_text(eng, P4_RDMA_PROG) == 0, "the RDMA program loads");

    /* No entries: nothing matches, no action runs, nothing is suppressed. */
    check(p4_engine_process_rdma(eng, NULL, &meta, &suppress) == 0,
          "the pipeline runs over an operation");
    check(!suppress,
          "an empty table suppresses nothing, which is what keeps a device "
          "with no control plane working");

    /* An entry for this opcode, whose action drops. */
    memset(&up, 0, sizeof(up));
    up.type = P4_UPDATE_INSERT;
    (void)snprintf(up.table, sizeof(up.table), "t");
    (void)snprintf(up.action, sizeof(up.action), "nope");
    up.nmatch = 1;
    up.match[0].len = 1;
    up.match[0].value[P4_MATCH_BYTES - 1] = 4;
    checkf(p4_engine_update(eng, &up, err, sizeof(err)) == 0,
           "the entry is not installed: %s", err);

    check(p4_engine_process_rdma(eng, NULL, &meta, &suppress) == 0,
          "the pipeline runs again");
    check(suppress, "the entry's action suppressed the operation");

    /* An operation whose opcode is not in the table is untouched. */
    meta.opcode = 5;
    check(p4_engine_process_rdma(eng, NULL, &meta, &suppress) == 0,
          "another operation runs");
    check(!suppress, "an opcode with no entry is not suppressed");

    /* An Ethernet-only program is not for this path at all. */
    check(load_fixture(eng, "loopback.json") == 0, "an Ethernet program "
                                                   "loads");
    check(p4_engine_process_rdma(eng, NULL, &meta, &suppress) == -ENOENT,
          "a program with no rdma_metadata is refused on the RDMA path");
    check(!suppress, "a refused run suppresses nothing");

    check(p4_engine_process_rdma(NULL, NULL, &meta, &suppress) == -EINVAL,
          "a NULL engine is refused");

    p4_engine_destroy(eng);
}

/* Records what the pipeline asked of the extern. */
struct extern_log {
    unsigned calls;
    uint8_t type;
    uint8_t method;
    uint8_t nargs;
    uint8_t op;
    int fail_with;
};

static int extern_record(void *ctx, uint8_t type, uint8_t method,
                         const struct p4_value *args, uint8_t nargs)
{
    struct extern_log *l = ctx;

    l->calls++;
    l->type = type;
    l->method = method;
    l->nargs = nargs;
    if (nargs)
        l->op = args[0].b[P4_VALUE_BYTES - 1];
    return l->fail_with;
}

/*
 * A P4 program deciding whether an operation is reduced and with which
 * arithmetic. This is the whole point of the exercise: the table says
 * what happens, the extern does it, and the program chose both.
 */
static void test_extern_call(void)
{
    struct p4_engine *eng = p4_engine_create();
    struct extern_log log;
    struct p4_extern_ops ops;
    struct p4_rdma_meta meta;
    struct p4_update up;
    bool suppress = false;
    char err[160];

    memset(&log, 0, sizeof(log));
    ops.call = extern_record;
    ops.ctx = &log;

    memset(&meta, 0, sizeof(meta));
    meta.opcode = 4;

    check(load_fixture(eng, "fp8_policy.json") == 0,
          "the compiled policy program loads");

    /* No entry yet: the table's default action does nothing. */
    check(p4_engine_process_rdma(eng, &ops, &meta, &suppress) == 0,
          "an operation runs with no entry installed");
    check(log.calls == 0, "nothing was asked of the extern");
    check(!suppress, "and nothing was suppressed");

    /* An entry saying: for this opcode, reduce with operation 1. */
    memset(&up, 0, sizeof(up));
    up.type = P4_UPDATE_INSERT;
    (void)snprintf(up.table, sizeof(up.table), "Ing.policy");
    (void)snprintf(up.action, sizeof(up.action), "Ing.reduce_with");
    up.nmatch = 1;
    up.match[0].len = 1;
    up.match[0].value[P4_MATCH_BYTES - 1] = 4;
    up.nparams = 1;
    up.params[0].len = 1;
    up.params[0].value[P4_MATCH_BYTES - 1] = P4_FP8_OP_MUL;
    checkf(p4_engine_update(eng, &up, err, sizeof(err)) == 0,
           "the entry is not installed: %s", err);

    check(p4_engine_process_rdma(eng, &ops, &meta, &suppress) == 0,
          "the operation runs again");
    check(log.calls == 1, "the extern was called once");
    check(log.method == P4_EXTERN_FP8_MAP, "it was asked for the map method");
    check(log.nargs == 1, "it was given one argument");
    check(log.op == P4_FP8_OP_MUL,
          "the argument is the operation the table entry chose");
    /* The extern wrote the destination, so the operation must not also
     * transfer; the action marks the drop and this is where that shows. */
    check(suppress, "an action that called the extern did not suppress");

    /* An opcode with no entry still does nothing. */
    log.calls = 0;
    meta.opcode = 7;
    check(p4_engine_process_rdma(eng, &ops, &meta, &suppress) == 0,
          "another operation runs");
    check(log.calls == 0, "an opcode with no entry asks nothing");

    /*
     * An extern that fails must fail the operation. Without this the work
     * request would complete successfully with the reduction not done,
     * which is worse than refusing it.
     */
    log.calls = 0;
    log.fail_with = -EFAULT;
    meta.opcode = 4;
    suppress = true;
    check(p4_engine_process_rdma(eng, &ops, &meta, &suppress) == -EFAULT,
          "a failing extern fails the operation");
    check(log.calls == 1, "it was still called");
    /* The action drops after the call, so the drop was never reached. */
    check(!suppress, "a drop the pipeline never reached was reported");
    log.fail_with = 0;

    /*
     * With no way to reach the operation's regions there is nothing to
     * work on, so the call fails rather than being skipped.
     */
    check(p4_engine_process_rdma(eng, NULL, &meta, &suppress) == -EPERM,
          "a call with no operand context is refused");

    p4_engine_destroy(eng);
}

/*
 * A drop made before an extern call that then fails is still reported.
 *
 * The datapath treats one failure -- the extern declining a request that is
 * not its shape -- as "no decision" and lets the operation through, so the
 * engine has to say whether the program had already dropped it.  Losing
 * that let a write the program denied be dispatched anyway.
 */
static void test_extern_after_drop(void)
{
    struct p4_engine *eng = p4_engine_create();
    struct extern_log log;
    struct p4_extern_ops ops;
    struct p4_rdma_meta meta;
    struct p4_update up;
    bool suppress = false;
    char err[160];

    memset(&log, 0, sizeof(log));
    ops.call = extern_record;
    ops.ctx = &log;

    memset(&meta, 0, sizeof(meta));
    meta.opcode = 4;

    check(load_fixture(eng, "fp8_drop_then_map.json") == 0,
          "the compiled drop-then-map program loads");

    memset(&up, 0, sizeof(up));
    up.type = P4_UPDATE_INSERT;
    (void)snprintf(up.table, sizeof(up.table), "Ing.policy");
    (void)snprintf(up.action, sizeof(up.action), "Ing.drop_then_reduce");
    up.nmatch = 1;
    up.match[0].len = 1;
    up.match[0].value[P4_MATCH_BYTES - 1] = 4;
    up.nparams = 1;
    up.params[0].len = 1;
    up.params[0].value[P4_MATCH_BYTES - 1] = P4_FP8_OP_ADD;
    checkf(p4_engine_update(eng, &up, err, sizeof(err)) == 0,
           "the entry is not installed: %s", err);

    log.fail_with = -ENOEXEC;
    check(p4_engine_process_rdma(eng, &ops, &meta, &suppress) == -ENOEXEC,
          "the extern's error is returned");
    check(log.calls == 1, "the extern was called");
    check(suppress, "the drop made before the call was lost");

    /* An opcode with no entry runs no action, error or not. */
    log.calls = 0;
    meta.opcode = 7;
    suppress = true;
    check(p4_engine_process_rdma(eng, &ops, &meta, &suppress) == 0,
          "an operation with no entry runs");
    check(log.calls == 0 && !suppress,
          "an operation with no entry was touched");

    p4_engine_destroy(eng);
}

int main(void)
{
    test_lifecycle();
    test_load_parses();
    test_load_real_artifacts();
    test_execute();
    test_execute_egress();
    test_execute_no_egress_assignment();
    test_execute_wide_field();
    test_table_entries();
    test_table_entry_errors();
    test_rdma_metadata();
    test_rdma_execute();
    test_extern_call();
    test_extern_after_drop();
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
