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

#include <json-c/json_object.h>
/* json_object_object_foreach expands to the link-hash walkers. */
#include <json-c/linkhash.h> // IWYU pragma: keep
#include <json-c/json_tokener.h>
#include <json-c/json_types.h>

#include "p4_engine.h"
#include "p4_exec.h"
#include "p4_json.h"
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

/* Why the last load was refused, for a failure message. */
static const char *load_why(const struct p4_engine *eng)
{
    const char *why = p4_engine_load_error(eng);

    return why ? why : "(no reason given)";
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
    check(strstr(load_why(eng), "32 of 96 bytes") != NULL,
          "the refusal does not say how much of the program arrived");

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
    /* Metadata is not on the wire, so a parser has no business filling it
     * from the frame. */
    check(load_text(eng, P4_HDRS "\"parsers\":[{\"name\":\"p\",\"id\":0,"
                                 "\"init_state\":\"start\",\"parse_states\":["
                                 "{\"name\":\"start\",\"id\":0,"
                                 "\"parser_ops\":[{\"op\":\"extract\","
                                 "\"parameters\":[{\"type\":\"regular\","
                                 "\"value\":\"standard_metadata\"}]}],"
                                 "\"transition_key\":[],"
                                 "\"transitions\":[]}]}],"
                                 "\"deparsers\":[]}") == -EINVAL,
          "extracting a metadata header is refused");
    check(strstr(p4_engine_load_error(eng), "which is metadata") != NULL,
          "the error says the header is metadata");

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
 * What the loader's scan should make of a document, weighed from the tree
 * json-c built rather than from the text, so the two are independent. With
 * an explicit stack, not recursion, and SIZE_MAX if that overflows.
 */
static size_t tree_cost(struct json_object *root)
{
    struct json_object *stack[4096];
    size_t n = 0, cost = 0;

    stack[n++] = root;
    while (n > 0) {
        struct json_object *o = stack[--n];

        if (json_object_is_type(o, json_type_object)) {
            cost += P4_JSON_COST_OBJECT;
            json_object_object_foreach(o, key, v)
            {
                (void)key;
                cost += P4_JSON_COST_OTHER;
                if (n == sizeof(stack) / sizeof(stack[0]))
                    return SIZE_MAX;
                stack[n++] = v;
            }
        } else if (json_object_is_type(o, json_type_array)) {
            cost += P4_JSON_COST_ARRAY;
            for (size_t i = 0; i < json_object_array_length(o); i++) {
                if (n == sizeof(stack) / sizeof(stack[0]))
                    return SIZE_MAX;
                stack[n++] = json_object_array_get_idx(o, i);
            }
        } else {
            cost += P4_JSON_COST_OTHER; /* null is a NULL object */
        }
    }
    return cost;
}

/*
 * @base (@base_len bytes, a JSON object weighing @base_cost) with an
 * ignored key added, "pad", whose array of copies of @elem (each weighing
 * @elem_cost) and then zeros brings the whole to P4_JSON_MAX_COST + @over.
 * NULL if @base doesn't end in '}' or the result is over P4_PROGRAM_MAX.
 */
static char *pad_to_budget(const char *base, size_t base_len, size_t base_cost,
                           const char *elem, size_t elem_cost, size_t over,
                           size_t *out_len)
{
    static const char key[] = ",\"pad\":[";
    size_t fixed = base_cost + P4_JSON_COST_OTHER + P4_JSON_COST_ARRAY;
    size_t elem_len = strlen(elem);
    size_t want = P4_JSON_MAX_COST + over;
    size_t nelem, nzero, cap, n;
    char *doc;

    while (base_len > 0 &&
           (base[base_len - 1] == ' ' || base[base_len - 1] == '\t' ||
            base[base_len - 1] == '\r' || base[base_len - 1] == '\n'))
        base_len--;
    if (base_len == 0 || base[base_len - 1] != '}' || fixed > want)
        return NULL;
    base_len--;
    nelem = (want - fixed) / elem_cost;
    nzero = (want - fixed) % elem_cost;

    /* Every element with a comma after it, then "]}". */
    cap = base_len + sizeof(key) + nelem * (elem_len + 1) + nzero * 2 + 2;
    if (cap > P4_PROGRAM_MAX)
        return NULL;
    doc = malloc(cap);
    if (!doc)
        return NULL;
    memcpy(doc, base, base_len);
    n = base_len;
    memcpy(doc + n, key, sizeof(key) - 1);
    n += sizeof(key) - 1;
    for (size_t i = 0; i < nelem; i++) {
        memcpy(doc + n, elem, elem_len);
        n += elem_len;
        doc[n++] = ',';
    }
    for (size_t i = 0; i < nzero; i++) {
        doc[n++] = '0';
        doc[n++] = ',';
    }
    if (nelem + nzero > 0)
        n--; /* the last comma */
    doc[n++] = ']';
    doc[n++] = '}';
    *out_len = n;
    return doc;
}

/*
 * The budget on what json-c builds. A real program padded with an ignored
 * key to exactly P4_JSON_MAX_COST loads, and one unit more is refused before
 * json-c sees it, for each kind of value. Exactly, because the scan's count
 * of the program is checked against json-c's own tree of it.
 */
static void test_load_cost_budget(void)
{
    static const struct {
        const char *elem;
        size_t cost;
    } pads[] = {
        {"0", P4_JSON_COST_OTHER},
        {"{}", P4_JSON_COST_OBJECT},
        {"[]", P4_JSON_COST_ARRAY},
        /* Brackets and an escaped quote in a string weigh nothing. */
        {"\"\\\"{[\"", P4_JSON_COST_OTHER},
        /*
         * json-c 0.18 accepts a single-quoted key even when STRICT, and the
         * double quote in it is literal. A scan that took it for the start
         * of a string would skip to the next element's and miss the rest.
         */
        {"{'\"':[]}",
         P4_JSON_COST_OBJECT + P4_JSON_COST_OTHER + P4_JSON_COST_ARRAY},
    };
    struct p4_program *prog = NULL;
    struct json_tokener *tok;
    struct json_object *root;
    size_t n = 0, base_cost, dlen = 0;
    char err[256];
    char *base, *doc;
    int rc;

    base = read_fixture("minimal_forward.json", &n);
    if (!base) {
        check(false, "minimal_forward.json could not be read");
        return;
    }
    tok = json_tokener_new();
    if (!tok) {
        check(false, "json_tokener_new failed");
        free(base);
        return;
    }
    root = json_tokener_parse_ex(tok, base, (int)n);
    json_tokener_free(tok);
    if (!root) {
        check(false, "minimal_forward.json is not JSON");
        free(base);
        return;
    }
    base_cost = tree_cost(root);
    json_object_put(root);
    if (base_cost >= P4_JSON_MAX_COST) {
        checkf(false, "minimal_forward.json weighs %zu", base_cost);
        free(base);
        return;
    }

    for (size_t i = 0; i < sizeof(pads) / sizeof(pads[0]); i++) {
        for (size_t over = 0; over <= 1; over++) {
            doc = pad_to_budget(base, n, base_cost, pads[i].elem, pads[i].cost,
                                over, &dlen);
            if (!doc) {
                checkf(false, "padding with %s could not be built",
                       pads[i].elem);
                continue;
            }
            rc = p4_json_parse(doc, dlen, &prog, err, sizeof(err));
            if (over == 0) {
                checkf(rc == 0, "padded with %s to the budget: %d (%s)",
                       pads[i].elem, rc, err);
                if (rc == 0)
                    p4_json_free(prog);
            } else {
                checkf(rc == -EINVAL &&
                           strstr(err, "too many JSON values") != NULL,
                       "padded with %s past the budget: %d (%s)", pads[i].elem,
                       rc, err);
            }
            free(doc);
        }
    }
    free(base);

    /* The document that would have taken a gigabyte: refused unparsed. */
    doc = malloc(P4_PROGRAM_MAX);
    if (!doc) {
        check(false, "out of memory");
        return;
    }
    n = 0;
    memcpy(doc, "{\"a\":[", 6);
    n += 6;
    while (n + 3 + 2 <= P4_PROGRAM_MAX) {
        memcpy(doc + n, "{},", 3);
        n += 3;
    }
    memcpy(doc + n - 1, "]}", 2);
    n++;
    rc = p4_json_parse(doc, n, &prog, err, sizeof(err));
    checkf(rc == -EINVAL && strstr(err, "too many JSON values") != NULL,
           "a document of empty objects: %d (%s)", rc, err);
    free(doc);
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
#define P4_RDMA_PROG P4_RDMA_HDRS P4_RDMA_TAIL
#define P4_RDMA_TAIL                                     \
    "\"actions\":[{\"name\":\"nope\",\"id\":0,"          \
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
 * A program may declare only the parts of the architecture it reads: here
 * no ingress_port and, of rdma_metadata, only the opcode.  The pipeline
 * fills in what was declared and skips what was not.
 */
#define P4_RDMA_FEW_HDRS                                                \
    "{\"__meta__\":{\"version\":[2,24]},"                               \
    "\"header_types\":[{\"name\":\"std_t\",\"id\":0,"                   \
    "\"fields\":[[\"egress_spec\",9]]},"                                \
    "{\"name\":\"rdma_t\",\"id\":1,\"fields\":[[\"opcode\",8]]}],"      \
    "\"headers\":[{\"name\":\"standard_metadata\",\"id\":0,"            \
    "\"header_type\":\"std_t\",\"metadata\":true},"                     \
    "{\"name\":\"rdma_metadata\",\"id\":1,"                             \
    "\"header_type\":\"rdma_t\",\"metadata\":true}],"                   \
    "\"parsers\":[{\"name\":\"p\",\"id\":0,\"init_state\":\"start\","   \
    "\"parse_states\":[{\"name\":\"start\",\"id\":0,\"parser_ops\":[]," \
    "\"transition_key\":[],\"transitions\":[{\"value\":\"default\","    \
    "\"mask\":null,\"next_state\":null}]}]}],"                          \
    "\"deparsers\":[{\"name\":\"d\",\"id\":0,\"order\":[]}],"

static void test_rdma_few_fields(void)
{
    struct p4_engine *eng = p4_engine_create();
    struct p4_rdma_meta meta;
    struct p4_update up;
    bool suppress = false;
    char err[160];

    checkf(load_text(eng, P4_RDMA_FEW_HDRS P4_RDMA_TAIL) == 0,
           "a program declaring only an opcode is refused: %s", load_why(eng));
    memset(&up, 0, sizeof(up));
    up.type = P4_UPDATE_INSERT;
    (void)snprintf(up.table, sizeof(up.table), "t");
    (void)snprintf(up.action, sizeof(up.action), "nope");
    up.nmatch = 1;
    up.match[0].len = 1;
    up.match[0].value[P4_MATCH_BYTES - 1] = 4;
    checkf(p4_engine_update(eng, &up, err, sizeof(err)) == 0,
           "the entry is not installed: %s", err);

    memset(&meta, 0, sizeof(meta));
    meta.opcode = 4;
    meta.qp_num = 7;
    meta.length = 4096;
    check(p4_engine_process_rdma(eng, NULL, &meta, &suppress) == 0 && suppress,
          "the opcode the program declared was not matched");

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

    /*
     * A frame has no operands either.  rdma_metadata is not valid on the
     * frame path, so its opcode reads as 0, and an entry for opcode 0
     * reaches the call: the frame is dropped rather than half run.
     */
    up.match[0].value[P4_MATCH_BYTES - 1] = 0;
    checkf(p4_engine_update(eng, &up, err, sizeof(err)) == 0,
           "the entry for opcode 0 is not installed: %s", err);
    {
        uint8_t in[64], out[64];
        struct p4_result res;

        fill_frame(in, sizeof(in));
        check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out,
                                sizeof(out), &res) == 0,
              "a frame runs into the extern call");
        check(res.verdict == P4_VERDICT_DROP,
              "a frame whose action calls an extern was not dropped");
    }

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

/* ---- The reject corpus -------------------------------------------------- */

/*
 * A case marked "encoding": "latin1" carries bytes no JSON string can: each
 * code point of its "json" is one byte of the document, so U+00FF is the
 * byte 0xFF rather than its UTF-8 encoding.  json-c hands the string back
 * as UTF-8, where a code point from U+0080 to U+00FF is two bytes: 0xC2 or
 * 0xC3, then a continuation byte.
 */
static bool latin1_decode(const char *s, size_t n, uint8_t *out,
                          size_t *out_len)
{
    size_t i = 0, o = 0;

    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        uint8_t next;

        if (c < 0x80u) {
            out[o++] = c;
            i++;
            continue;
        }
        if ((c != 0xC2u && c != 0xC3u) || i + 1 >= n)
            return false;
        next = (uint8_t)s[i + 1];
        if ((next & 0xC0u) != 0x80u)
            return false;
        out[o++] = (uint8_t)(((c & 0x03u) << 6) | (next & 0x3Fu));
        i += 2;
    }
    *out_len = o;
    return true;
}

/* A string member of a case, or NULL, with its length, NULs and all. */
static const char *case_str(const struct json_object *c, const char *key,
                            size_t *len)
{
    struct json_object *v;

    if (!json_object_object_get_ex(c, key, &v) ||
        !json_object_is_type(v, json_type_string))
        return NULL;
    if (len)
        *len = (size_t)json_object_get_string_len(v);
    return json_object_get_string(v);
}

/* One case of the corpus: load it and check the refusal it names. */
static void reject_case(const struct json_object *c, size_t i)
{
    const char *name = case_str(c, "name", NULL);
    const char *enc = case_str(c, "encoding", NULL);
    const char *want_errno = case_str(c, "errno", NULL);
    const char *want = case_str(c, "error", NULL);
    size_t jlen = 0, dlen = 0;
    const char *json = case_str(c, "json", &jlen);
    struct p4_program *prog = NULL;
    char err[256];
    uint8_t *doc;
    int want_rc, rc;

    if (!name || !json || !want_errno || !want) {
        checkf(false, "reject case %zu is missing a member", i);
        return;
    }
    if (strcmp(want_errno, "EINVAL") == 0) {
        want_rc = -EINVAL;
    } else if (strcmp(want_errno, "ENOSYS") == 0) {
        want_rc = -ENOSYS;
    } else {
        checkf(false, "%s: unknown errno '%s'", name, want_errno);
        return;
    }

    /* A byte more than the text, so the empty case still has a buffer. */
    doc = malloc(jlen + 1);
    if (!doc) {
        checkf(false, "%s: out of memory", name);
        return;
    }
    if (enc) {
        if (strcmp(enc, "latin1") != 0 ||
            !latin1_decode(json, jlen, doc, &dlen)) {
            checkf(false, "%s: cannot decode it as '%s'", name, enc);
            free(doc);
            return;
        }
    } else {
        memcpy(doc, json, jlen);
        dlen = jlen;
    }

    rc = p4_json_parse(doc, dlen, &prog, err, sizeof(err));
    checkf(rc == want_rc, "%s: returned %d, want %d (%s)", name, rc, want_rc,
           err);
    checkf(strstr(err, want) != NULL, "%s: refused with \"%s\", want \"%s\"",
           name, err, want);
    /* The reason reaches the log, so a name echoed into it must not carry
     * a newline or an escape from the document. */
    checkf(p4_name_printable(err, strlen(err)),
           "%s: the refusal is not printable", name);
    if (rc == 0)
        p4_json_free(prog);
    free(doc);
}

/*
 * reject_cases.json: hand-written documents, each skeleton.json with one
 * defect, and the refusal the loader must give -- its errno and a piece of
 * its reason.  The reason is the point: a case refused for an earlier
 * defect never reaches the check it was written for.  39 of these once
 * passed that way, refused only because the skeleton had no
 * standard_metadata, which the loader came to require after the corpus was
 * written.
 */
static void test_reject_corpus(void)
{
    struct json_tokener *tok;
    struct json_object *root;
    struct p4_program *prog = NULL;
    char err[256];
    size_t n = 0, ncases;
    char *buf;

    /* The skeleton itself loads, so the defect is the only thing that can
     * refuse a case built on it. */
    buf = read_fixture("skeleton.json", &n);
    if (!buf) {
        check(false, "skeleton.json could not be read");
        return;
    }
    checkf(p4_json_parse(buf, n, &prog, err, sizeof(err)) == 0,
           "skeleton.json is refused: %s", err);
    p4_json_free(prog);
    free(buf);

    buf = read_fixture("reject_cases.json", &n);
    if (!buf) {
        check(false, "reject_cases.json could not be read");
        return;
    }
    tok = json_tokener_new();
    if (!tok) {
        check(false, "json_tokener_new failed");
        free(buf);
        return;
    }
    root = json_tokener_parse_ex(tok, buf, (int)n);
    if (!root || !json_object_is_type(root, json_type_array)) {
        check(false, "reject_cases.json is not a JSON array");
    } else {
        ncases = json_object_array_length(root);
        check(ncases > 0, "reject_cases.json has no cases");
        for (size_t i = 0; i < ncases; i++)
            reject_case(json_object_array_get_idx(root, i), i);
    }
    json_object_put(root);
    json_tokener_free(tok);
    free(buf);
}

/*
 * A parse state that loops is accepted.  A P4 parser may revisit a state --
 * a stack of VLAN tags is parsed that way -- so the loader leaves a cycle
 * in the parser alone, unlike one in the pipeline.  What stops it spinning
 * is the executor's bound on parser steps, and a frame that reaches the
 * bound is dropped.  This parser loops without consuming anything, so
 * every frame does.
 */
#define P4_LOOP_PROG                                                        \
    P4_HDRS "\"parsers\":[{\"name\":\"p\",\"id\":0,"                        \
            "\"init_state\":\"start\",\"parse_states\":["                   \
            "{\"name\":\"start\",\"id\":0,\"parser_ops\":[],"               \
            "\"transition_key\":[],"                                        \
            "\"transitions\":[{\"value\":\"default\",\"mask\":null,"        \
            "\"next_state\":\"start\"}]}]}],"                               \
            "\"deparsers\":[{\"name\":\"d\",\"id\":0,\"order\":[]}],"       \
            "\"actions\":[],\"pipelines\":[{\"name\":\"ingress\",\"id\":0," \
            "\"init_table\":null,\"tables\":[],\"action_profiles\":[],"     \
            "\"conditionals\":[]}]}"

static void test_parser_loop(void)
{
    struct p4_engine *eng = p4_engine_create();
    uint8_t in[64], out[64];
    struct p4_result res;
    struct p4_stats st;

    fill_frame(in, sizeof(in));
    checkf(load_text(eng, P4_LOOP_PROG) == 0,
           "a parse state that loops to itself is refused: %s", load_why(eng));
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            &res) == 0,
          "a frame runs into the looping parser");
    check(res.verdict == P4_VERDICT_DROP,
          "a frame that never leaves the parser is dropped");
    p4_engine_get_stats(eng, &st);
    check(st.frames_dropped == 1 && st.parse_errors == 0,
          "the bound is a drop, not a malformed frame");

    p4_engine_destroy(eng);
}

/*
 * A parser that extracts one header twice.  The second extract overwrites
 * the first, so the header is valid once but the parser read two headers'
 * worth of the frame, and the payload starts after both: bmv2 emits the
 * second copy and then the payload, dropping the first.  Working out where
 * the payload starts from the valid headers counted one, and put the
 * second copy on the wire twice.
 */
#define P4_TWICE_PROG                                                        \
    P4_HDRS "\"parsers\":[{\"name\":\"p\",\"id\":0,"                         \
            "\"init_state\":\"start\",\"parse_states\":["                    \
            "{\"name\":\"start\",\"id\":0,\"parser_ops\":["                  \
            "{\"op\":\"extract\",\"parameters\":[{\"type\":\"regular\","     \
            "\"value\":\"eth\"}]},"                                          \
            "{\"op\":\"extract\",\"parameters\":[{\"type\":\"regular\","     \
            "\"value\":\"eth\"}]}],"                                         \
            "\"transition_key\":[],"                                         \
            "\"transitions\":[{\"value\":\"default\",\"mask\":null,"         \
            "\"next_state\":null}]}]}],"                                     \
            "\"deparsers\":[{\"name\":\"d\",\"id\":0,\"order\":[\"eth\"]}]," \
            "\"actions\":[],\"pipelines\":[{\"name\":\"ingress\",\"id\":0,"  \
            "\"init_table\":null,\"tables\":[],\"action_profiles\":[],"      \
            "\"conditionals\":[]}]}"

static void test_parser_extract_twice(void)
{
    struct p4_engine *eng = p4_engine_create();
    uint8_t in[64], out[128];
    struct p4_result res;

    fill_frame(in, sizeof(in));
    checkf(load_text(eng, P4_TWICE_PROG) == 0,
           "a parser extracting one header twice is refused: %s",
           load_why(eng));
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(out),
                            &res) == 0,
          "a frame runs through the parser");
    check(res.verdict == P4_VERDICT_FORWARD, "the frame is forwarded");
    check(res.len == sizeof(in) - 14 &&
              memcmp(out, in + 14, sizeof(in) - 14) == 0,
          "the first copy of the header was not dropped, as bmv2 does");

    p4_engine_destroy(eng);
}

/*
 * Two members with one name: json-c keeps the last, and the loader takes
 * what json-c gives it, so the second __meta__ is the one checked.  p4c
 * never writes a duplicate; this pins down what happens when something
 * else does, so that changing it is a decision rather than an accident.
 */
static void test_duplicate_keys(void)
{
    struct p4_engine *eng = p4_engine_create();
    /* A whole program, less its opening brace, to put a member before. */
    const char *rest = &P4_TO_PORT("0x1")[1];
    char doc[4096];

    (void)snprintf(doc, sizeof(doc), "{\"__meta__\":{\"version\":[9,9]},%s",
                   rest);
    checkf(load_text(eng, doc) == 0,
           "a document whose last __meta__ is good is refused: %s",
           load_why(eng));

    /* The other way round, the version 9 one is checked: had the first
     * been, this would get past it and be refused for having nothing
     * else. */
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},"
                         "\"__meta__\":{\"version\":[9,9]}}") == -ENOSYS,
          "a document whose last __meta__ is version 9 was not refused");

    p4_engine_destroy(eng);
}

/*
 * A deparser may emit a metadata header, which adds bytes the frame did
 * not have.  Bytes the output buffer cannot hold are refused, whether it
 * is a header or the payload after one that does not fit.
 */
#define P4_DEPARSE(order)                                                 \
    P4_PARSER "\"deparsers\":[{\"name\":\"d\",\"id\":0,\"order\":[" order \
              "]}],\"actions\":[],\"pipelines\":[{\"name\":\"ingress\","  \
              "\"id\":0,\"init_table\":null,\"tables\":[],"               \
              "\"action_profiles\":[],\"conditionals\":[]}]}"

static void test_deparse_overflow(void)
{
    struct p4_engine *eng = p4_engine_create();
    uint8_t in[64], out[64];
    struct p4_result res;
    struct p4_stats st;

    fill_frame(in, sizeof(in));

    /* Ethernet and the 3 bytes of standard_metadata fit; the payload,
     * now 3 bytes further on, does not. */
    checkf(load_text(eng, P4_DEPARSE("\"eth\",\"standard_metadata\"")) == 0,
           "a deparser emitting metadata is refused: %s", load_why(eng));
    check(p4_engine_process(eng, P4_PORT_HOST, in, sizeof(in), out, sizeof(in),
                            &res) == 0,
          "the frame runs");
    check(res.verdict == P4_VERDICT_DROP,
          "a payload pushed past the output buffer is dropped");

    /* The metadata first, so it is Ethernet that does not fit. */
    checkf(load_text(eng, P4_DEPARSE("\"standard_metadata\",\"eth\"")) == 0,
           "a deparser emitting metadata first is refused: %s", load_why(eng));
    check(p4_engine_process(eng, P4_PORT_HOST, in, 14, out, 14, &res) == 0,
          "the frame runs");
    check(res.verdict == P4_VERDICT_DROP,
          "a header pushed past the output buffer is dropped");

    /* With room for both, the metadata really is on the front. */
    check(p4_engine_process(eng, P4_PORT_HOST, in, 14, out, sizeof(out),
                            &res) == 0,
          "the frame runs");
    check(res.verdict == P4_VERDICT_FORWARD && res.len == 17 &&
              memcmp(out + 3, in, 14) == 0,
          "the deparser did not put 3 bytes of metadata before Ethernet");

    p4_engine_get_stats(eng, &st);
    check(st.frames_dropped == 2, "both overflows were counted as drops");

    p4_engine_destroy(eng);
}

/* ---- features.json: what no compiled fixture uses ----------------------- */

/* A frame under construction, in network byte order. */
struct frame {
    uint8_t b[256];
    size_t len;
};

static void put_be(struct frame *f, uint64_t v, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++)
        f->b[f->len++] = (uint8_t)(v >> (8u * (bytes - 1u - i)));
}

static void frame_eth(struct frame *f, uint16_t type)
{
    f->len = 0;
    put_be(f, UINT64_C(0x020000000001), 6);
    put_be(f, UINT64_C(0x020000000002), 6);
    put_be(f, type, 2);
}

/* A tag with priority and DEI zero, which the program does not read. */
static void frame_vlan(struct frame *f, uint16_t vid, uint16_t type)
{
    put_be(f, vid & 0x0FFFu, 2);
    put_be(f, type, 2);
}

/* Where frame_ipv4() puts the fields dec_ttl changes, from the header. */
#define IP4_DIFFSERV 1u
#define IP4_ID       4u
#define IP4_TTL      8u
#define IP4_CSUM     10u

/* An IPv4 header, then 16 bytes of payload. */
static void frame_ipv4(struct frame *f, uint32_t src, uint32_t dst, uint8_t ttl,
                       uint8_t proto)
{
    put_be(f, 0x45u, 1); /* version 4, IHL 5 */
    put_be(f, 0x10u, 1); /* diffserv */
    put_be(f, 36u, 2);   /* total length */
    put_be(f, 0x1234u, 2);
    put_be(f, 0u, 2); /* flags, fragment offset */
    put_be(f, ttl, 1);
    put_be(f, proto, 1);
    put_be(f, 0u, 2); /* checksum */
    put_be(f, src, 4);
    put_be(f, dst, 4);
    for (unsigned i = 0; i < 16; i++)
        put_be(f, 0xA0u + i, 1);
}

#define IP4(a, b, c, d)                                                     \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | \
     (uint32_t)(d))

#define PROTO_TCP 6u
#define PROTO_UDP 17u

/* Run @f in from the host port. */
static int run_frame(struct p4_engine *eng, const struct frame *f, uint8_t *out,
                     size_t out_cap, struct p4_result *res)
{
    memset(res, 0, sizeof(*res));
    return p4_engine_process(eng, P4_PORT_HOST, f->b, f->len, out, out_cap,
                             res);
}

/* Whether @f is forwarded, and to @port. */
static bool forwards_to(struct p4_engine *eng, const struct frame *f,
                        uint32_t port)
{
    uint8_t out[256];
    struct p4_result res;

    return run_frame(eng, f, out, sizeof(out), &res) == 0 &&
           res.verdict == P4_VERDICT_FORWARD && res.egress_port == port;
}

static bool drops(struct p4_engine *eng, const struct frame *f)
{
    uint8_t out[256];
    struct p4_result res;

    return run_frame(eng, f, out, sizeof(out), &res) == 0 &&
           res.verdict == P4_VERDICT_DROP;
}

/* An IPv4 frame, untagged, from @src to @dst. */
static void ip_frame(struct frame *f, uint32_t src, uint32_t dst, uint8_t ttl,
                     uint8_t proto)
{
    frame_eth(f, 0x0800u);
    frame_ipv4(f, src, dst, ttl, proto);
}

/* @v, right-aligned in the @len significant bytes of a match value. */
static void put_spec(uint8_t *dst, uint64_t v, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
        dst[P4_MATCH_BYTES - 1u - i] = (uint8_t)(v >> (8u * i));
}

/* An entry in the LPM table "route": @prefix/@plen to @port. */
static void route_entry(struct p4_update *up, uint8_t type, uint32_t prefix,
                        uint16_t plen, uint16_t port)
{
    memset(up, 0, sizeof(*up));
    up->type = type;
    (void)snprintf(up->table, sizeof(up->table), "route");
    (void)snprintf(up->action, sizeof(up->action), "fwd");
    up->nmatch = 1;
    up->match[0].len = 4;
    up->match[0].prefix_bits = plen;
    put_spec(up->match[0].value, prefix, 4);
    up->nparams = 1;
    up->params[0].len = 2;
    put_spec(up->params[0].value, port, 2);
}

/* An entry in the ternary table "acl", setting the class to @cls. */
static void acl_entry(struct p4_update *up, uint8_t type, uint32_t src,
                      uint32_t src_mask, uint8_t proto, uint8_t proto_mask,
                      uint32_t priority, uint8_t cls)
{
    memset(up, 0, sizeof(*up));
    up->type = type;
    (void)snprintf(up->table, sizeof(up->table), "acl");
    (void)snprintf(up->action, sizeof(up->action), "set_class");
    up->priority = priority;
    up->nmatch = 2;
    up->match[0].len = 4;
    put_spec(up->match[0].value, src, 4);
    put_spec(up->match[0].mask, src_mask, 4);
    up->match[1].len = 1;
    put_spec(up->match[1].value, proto, 1);
    put_spec(up->match[1].mask, proto_mask, 1);
    up->nparams = 1;
    up->params[0].len = 1;
    put_spec(up->params[0].value, cls, 1);
}

static int update(struct p4_engine *eng, const struct p4_update *up)
{
    char err[160];

    return p4_engine_update(eng, up, err, sizeof(err));
}

/*
 * The parser: a select on a field under a mask, a default arm, a state
 * that selects on two fields with no default, and a state that loops.
 *
 * parse_vlan selects on (vid, etherType): 12 bits and 16.  bmv2 gives each
 * field of a select key whole bytes of its own, and p4c writes the arms to
 * match, so VLAN 10 over IPv4 is 0x000a0800.  Packed bit to bit, the key
 * would be 0x00a08000, and no tagged frame would ever match.
 */
static void test_features_parser(void)
{
    struct p4_engine *eng = p4_engine_create();
    struct p4_update up;
    struct p4_stats st;
    struct frame f;
    uint8_t out[256];
    struct p4_result res;

    checkf(load_fixture(eng, "features.json") == 0,
           "features.json is refused: %s", load_why(eng));
    route_entry(&up, P4_UPDATE_INSERT, IP4(10, 0, 0, 0), 8, P4_PORT_UPLINK);
    check(update(eng, &up) == 0, "a route is not installed");

    /* Neither IPv4 nor tagged: the default arm, and the frame goes on
     * unchanged, to the other port, since nothing chose one. */
    frame_eth(&f, 0x88B5u);
    for (unsigned i = 0; i < 50; i++)
        put_be(&f, i, 1);
    check(run_frame(eng, &f, out, sizeof(out), &res) == 0 &&
              res.verdict == P4_VERDICT_FORWARD &&
              res.egress_port == P4_PORT_UPLINK && res.len == f.len &&
              memcmp(out, f.b, f.len) == 0,
          "an untagged non-IP frame was not passed through unchanged");

    /* Tagged, and not IPv4: the masked arm accepts it, and the pipeline
     * drops it, because valid(vlan). */
    frame_eth(&f, 0x8100u);
    frame_vlan(&f, 10, 0x88B5u);
    put_be(&f, 0, 8);
    check(drops(eng, &f), "a tagged non-IP frame was not dropped");

    /* VLAN 10 over IPv4: the two-field arm. */
    frame_eth(&f, 0x8100u);
    frame_vlan(&f, 10, 0x0800u);
    frame_ipv4(&f, IP4(192, 0, 2, 1), IP4(10, 1, 2, 3), 64, PROTO_TCP);
    check(forwards_to(eng, &f, P4_PORT_UPLINK),
          "VLAN 10 over IPv4 did not match its two-field arm");

    /* 0x9100 is 0x8100 under the mask 0xefff, and the second tag loops
     * back into parse_vlan. */
    frame_eth(&f, 0x9100u);
    frame_vlan(&f, 99, 0x8100u);
    frame_vlan(&f, 10, 0x0800u);
    frame_ipv4(&f, IP4(192, 0, 2, 1), IP4(10, 1, 2, 3), 64, PROTO_TCP);
    check(forwards_to(eng, &f, P4_PORT_UPLINK),
          "a 0x9100 tag over an 0x8100 one was not parsed through");

    /* VLAN 11 over IPv4 matches no arm, and parse_vlan has no default:
     * the parser rejects it. */
    p4_engine_get_stats(eng, &st);
    frame_eth(&f, 0x8100u);
    frame_vlan(&f, 11, 0x0800u);
    frame_ipv4(&f, IP4(192, 0, 2, 1), IP4(10, 1, 2, 3), 64, PROTO_TCP);
    check(drops(eng, &f), "a frame no arm matched was not dropped");
    {
        uint64_t before = st.parse_errors;

        p4_engine_get_stats(eng, &st);
        check(st.parse_errors == before + 1,
              "a frame no arm matched was not a parse error");
    }

    /* Forty tags: parse_vlan loops on every one, and the parser stops at
     * its bound long before the frame runs out. */
    frame_eth(&f, 0x8100u);
    for (unsigned i = 0; i < 40; i++)
        frame_vlan(&f, 1, 0x8100u);
    check(drops(eng, &f), "a frame looping in the parser was not dropped");

    p4_engine_destroy(eng);
}

/*
 * The actions on the way out: subtract_from_field, a three-operand add of
 * a runtime argument the table's default entry supplies, add_to_field, and
 * a three-operand subtract that wraps.  And the condition in front of them,
 * an or of two ==, which drops a frame whose TTL has run out.
 */
static void test_features_actions(void)
{
    struct p4_engine *eng = p4_engine_create();
    struct p4_update up;
    struct frame f;
    uint8_t out[256], want[256];
    struct p4_result res;
    const size_t ip = 14; /* where the IPv4 header starts */

    checkf(load_fixture(eng, "features.json") == 0,
           "features.json is refused: %s", load_why(eng));
    route_entry(&up, P4_UPDATE_INSERT, IP4(10, 0, 0, 0), 8, P4_PORT_UPLINK);
    check(update(eng, &up) == 0, "a route is not installed");

    ip_frame(&f, IP4(192, 0, 2, 1), IP4(10, 1, 2, 3), 64, PROTO_TCP);
    memcpy(want, f.b, f.len);
    want[ip + IP4_TTL] = 63;
    want[ip + IP4_ID] = 0x13; /* 0x1234 + the default entry's 0x0100 */
    want[ip + IP4_DIFFSERV] = 0x14;
    want[ip + IP4_CSUM] = 0xFF; /* 0 - 1, wrapped to 16 bits */
    want[ip + IP4_CSUM + 1] = 0xFF;
    check(run_frame(eng, &f, out, sizeof(out), &res) == 0 &&
              res.verdict == P4_VERDICT_FORWARD,
          "a routed frame was not forwarded");
    check(res.len == f.len && memcmp(out, want, f.len) == 0,
          "dec_ttl did not change the header as written");

    ip_frame(&f, IP4(192, 0, 2, 1), IP4(10, 1, 2, 3), 1, PROTO_TCP);
    check(drops(eng, &f), "a frame with a TTL of 1 was not dropped");
    ip_frame(&f, IP4(192, 0, 2, 1), IP4(10, 1, 2, 3), 0, PROTO_TCP);
    check(drops(eng, &f), "a frame with a TTL of 0 was not dropped");
    ip_frame(&f, IP4(192, 0, 2, 1), IP4(10, 1, 2, 3), 2, PROTO_TCP);
    check(forwards_to(eng, &f, P4_PORT_UPLINK),
          "a frame with a TTL of 2 was not forwarded");

    p4_engine_destroy(eng);
}

/*
 * The LPM table: the longest prefix wins whatever order the routes went
 * in, a /0 is a default route that loses to everything, and a row is
 * identified by what it matches rather than by its bytes.
 */
static void test_features_lpm(void)
{
    struct p4_engine *eng = p4_engine_create();
    struct p4_update up;
    struct frame to_10_1, to_10_2, to_192;

    checkf(load_fixture(eng, "features.json") == 0,
           "features.json is refused: %s", load_why(eng));
    ip_frame(&to_10_1, IP4(192, 0, 2, 1), IP4(10, 1, 2, 3), 64, PROTO_TCP);
    ip_frame(&to_10_2, IP4(192, 0, 2, 1), IP4(10, 2, 0, 1), 64, PROTO_TCP);
    ip_frame(&to_192, IP4(10, 0, 0, 5), IP4(192, 168, 0, 1), 64, PROTO_TCP);

    check(drops(eng, &to_10_1), "with no routes the default action drops");

    /* The shorter prefix first, so first-match would pick it. */
    route_entry(&up, P4_UPDATE_INSERT, IP4(10, 0, 0, 0), 8, P4_PORT_HOST);
    check(update(eng, &up) == 0, "10/8 is not installed");
    route_entry(&up, P4_UPDATE_INSERT, IP4(10, 1, 0, 0), 16, P4_PORT_UPLINK);
    check(update(eng, &up) == 0, "10.1/16 is not installed");

    check(forwards_to(eng, &to_10_1, P4_PORT_UPLINK),
          "10.1.2.3 did not take the /16 over the /8");
    check(forwards_to(eng, &to_10_2, P4_PORT_HOST), "10.2.0.1 did not take /8");
    check(drops(eng, &to_192), "a destination with no route was not dropped");

    route_entry(&up, P4_UPDATE_INSERT, 0, 0, P4_PORT_UPLINK);
    check(update(eng, &up) == 0, "a default route is not installed");
    check(forwards_to(eng, &to_192, P4_PORT_UPLINK),
          "the default route did not catch 192.168.0.1");
    check(forwards_to(eng, &to_10_2, P4_PORT_HOST),
          "the default route beat a /8");

    /* A host bit past the prefix is malformed, as in P4Runtime, not
     * another way of writing 10/8. */
    route_entry(&up, P4_UPDATE_INSERT, IP4(10, 0, 0, 1), 8, P4_PORT_UPLINK);
    check(update(eng, &up) == -EINVAL, "10.0.0.1/8 was not refused");
    route_entry(&up, P4_UPDATE_INSERT, IP4(172, 16, 0, 1), 8, P4_PORT_UPLINK);
    check(update(eng, &up) == -EINVAL, "172.16.0.1/8, a new row, was not "
                                       "refused");
    route_entry(&up, P4_UPDATE_DELETE, IP4(10, 0, 0, 1), 8, 0);
    check(update(eng, &up) == -EINVAL, "a delete of 10.0.0.1/8 was not "
                                       "refused");

    /* LPM takes no priority. */
    route_entry(&up, P4_UPDATE_INSERT, IP4(172, 16, 0, 0), 12, P4_PORT_UPLINK);
    up.priority = 1;
    check(update(eng, &up) == -EINVAL, "an LPM entry took a priority");
    route_entry(&up, P4_UPDATE_DELETE, IP4(10, 0, 0, 0), 8, 0);
    up.priority = 1;
    check(update(eng, &up) == -EINVAL, "an LPM delete took a priority");

    /* The same row, however it is written: a byte past the 4 that are
     * significant. */
    route_entry(&up, P4_UPDATE_INSERT, IP4(10, 0, 0, 0), 8, P4_PORT_UPLINK);
    up.match[0].value[0] = 0xAAu;
    check(update(eng, &up) == -EEXIST,
          "a byte the lookup never reads made a second 10/8");

    /* What does not fit is refused, not truncated. */
    route_entry(&up, P4_UPDATE_INSERT, IP4(172, 16, 0, 0), 33, P4_PORT_UPLINK);
    check(update(eng, &up) == -EINVAL, "a 33-bit IPv4 prefix was accepted");
    route_entry(&up, P4_UPDATE_INSERT, IP4(172, 16, 0, 0), 12, 0x3FFu);
    check(update(eng, &up) == -EINVAL,
          "a 10-bit port for a 9-bit parameter was accepted");

    /* Modify and delete find the row the same way. */
    route_entry(&up, P4_UPDATE_DELETE, IP4(10, 1, 0, 0), 16, 0);
    check(update(eng, &up) == 0, "10.1/16 is not deleted");
    check(forwards_to(eng, &to_10_1, P4_PORT_HOST),
          "10.1.2.3 did not fall back to the /8");
    route_entry(&up, P4_UPDATE_MODIFY, IP4(10, 0, 0, 0), 8, P4_PORT_UPLINK);
    check(update(eng, &up) == 0, "10/8 is not modified");
    check(forwards_to(eng, &to_10_1, P4_PORT_UPLINK),
          "the modified /8 did not send 10.1.2.3 to the uplink");
    route_entry(&up, P4_UPDATE_MODIFY, IP4(10, 1, 0, 0), 16, P4_PORT_UPLINK);
    check(update(eng, &up) == -ENOENT, "a route that is gone was modified");

    p4_engine_destroy(eng);
}

/*
 * An LPM table whose LPM key is not its first, the usual {vrf: exact;
 * dst: lpm} shape: here the EtherType stands in for the VRF.
 */
#define P4_VRF_PROG                                                     \
    P4_FRONT "\"actions\":[{\"name\":\"fwd\",\"id\":0,"                 \
             "\"runtime_data\":[{\"name\":\"port\",\"bitwidth\":9}],"   \
             "\"primitives\":[{\"op\":\"assign\",\"parameters\":["      \
             "{\"type\":\"field\",\"value\":[\"standard_metadata\","    \
             "\"egress_spec\"]},"                                       \
             "{\"type\":\"runtime_data\",\"value\":0}]}]}],"            \
             "\"pipelines\":[{\"name\":\"ingress\",\"id\":0,"           \
             "\"init_table\":\"vrf_route\",\"tables\":["                \
             "{\"name\":\"vrf_route\",\"id\":0,\"match_type\":\"lpm\"," \
             "\"type\":\"simple\",\"max_size\":16,"                     \
             "\"with_counters\":false,\"direct_meters\":null,"          \
             "\"support_timeout\":false,"                               \
             "\"key\":[{\"match_type\":\"exact\","                      \
             "\"target\":[\"eth\",\"ty\"],\"mask\":null},"              \
             "{\"match_type\":\"lpm\","                                 \
             "\"target\":[\"eth\",\"dst\"],\"mask\":null}],"            \
             "\"actions\":[\"fwd\"],\"next_tables\":{\"fwd\":null}}],"  \
             "\"action_profiles\":[],\"conditionals\":[]}]}"

/* An entry in P4_VRF_PROG's table: EtherType @ty, @dst/@plen, to @port. */
static void vrf_entry(struct p4_update *up, uint8_t type, uint16_t ty,
                      uint64_t dst, uint16_t plen, uint16_t port)
{
    memset(up, 0, sizeof(*up));
    up->type = type;
    (void)snprintf(up->table, sizeof(up->table), "vrf_route");
    (void)snprintf(up->action, sizeof(up->action), "fwd");
    up->nmatch = 2;
    up->match[0].len = 2;
    put_spec(up->match[0].value, ty, 2);
    up->match[1].len = 6;
    up->match[1].prefix_bits = plen;
    put_spec(up->match[1].value, dst, 6);
    up->nparams = 1;
    up->params[0].len = 2;
    put_spec(up->params[0].value, port, 2);
}

/*
 * The longest prefix wins by the LPM key's prefix, not by key 0's.  Ranked
 * by key 0, an exact field, every entry here ties at 0 and the first
 * inserted wins.  And a prefix on a field that is not LPM is refused, in
 * every kind of update, rather than kept where nothing reads it.
 */
static void test_lpm_key_not_first(void)
{
    struct p4_engine *eng = p4_engine_create();
    struct p4_update up;
    struct frame f;
    char err[160];

    checkf(load_text(eng, P4_VRF_PROG) == 0,
           "an LPM table with an exact key first is refused: %s",
           load_why(eng));

    /* To 02:00:00:00:00:01; padded past the minimum frame. */
    frame_eth(&f, 0x0800u);
    for (unsigned i = 0; i < 6; i++)
        put_be(&f, 0, 8);

    /* The shorter prefix first, so first-match would pick it. */
    vrf_entry(&up, P4_UPDATE_INSERT, 0x0800u, 0, 0, P4_PORT_UPLINK);
    check(update(eng, &up) == 0, "the /0 is not installed");
    vrf_entry(&up, P4_UPDATE_INSERT, 0x0800u, UINT64_C(0x020000000000), 8,
              P4_PORT_HOST);
    check(update(eng, &up) == 0, "the /8 is not installed");
    check(forwards_to(eng, &f, P4_PORT_HOST),
          "the /0, inserted first, beat the /8");

    /* A prefix on the exact field.  Each update would succeed without it:
     * the insert is a new row, and the modify and delete find the /0. */
    for (unsigned type = P4_UPDATE_INSERT; type <= P4_UPDATE_DELETE; type++) {
        if (type == P4_UPDATE_INSERT)
            vrf_entry(&up, (uint8_t)type, 0x0800u, UINT64_C(0x020100000000), 16,
                      P4_PORT_UPLINK);
        else
            vrf_entry(&up, (uint8_t)type, 0x0800u, 0, 0, P4_PORT_UPLINK);
        up.match[0].prefix_bits = 48;
        checkf(p4_engine_update(eng, &up, err, sizeof(err)) == -EINVAL,
               "update type %u took a prefix on an exact field", type);
        checkf(strstr(err, "not an LPM field") != NULL,
               "update type %u was refused with \"%s\"", type, err);
    }
    check(forwards_to(eng, &f, P4_PORT_HOST),
          "a refused update changed the table");

    /* A delete is held to the LPM field's width too, rather than finding
     * no such row. */
    vrf_entry(&up, P4_UPDATE_DELETE, 0x0800u, 0, 49, 0);
    check(update(eng, &up) == -EINVAL, "a delete of a /49 in a 48-bit "
                                       "field was not refused");

    p4_engine_destroy(eng);
}

/*
 * The ternary table: the highest priority wins whatever order the rows
 * went in, two rows may share a key at different priorities, and the
 * condition after it -- every comparison there is, and a bool -- reads
 * the class the winner set.  Classes 5 to 7 and 11 to 31 but not 12 are
 * dropped; everything else goes on.
 */
static void test_features_ternary(void)
{
    struct p4_engine *eng = p4_engine_create();
    struct p4_update up;
    struct frame f;

    checkf(load_fixture(eng, "features.json") == 0,
           "features.json is refused: %s", load_why(eng));
    route_entry(&up, P4_UPDATE_INSERT, 0, 0, P4_PORT_UPLINK);
    check(update(eng, &up) == 0, "a default route is not installed");

    /* A miss runs nop and takes __MISS__, straight to the TTL check. */
    ip_frame(&f, IP4(192, 168, 1, 5), IP4(10, 1, 2, 3), 64, PROTO_TCP);
    check(forwards_to(eng, &f, P4_PORT_UPLINK),
          "a frame the ACL missed was not forwarded");

    /* A: 192.168/16, any protocol, class 6, priority 10. B: 192.168.1/24,
     * class 12, priority 20. C: 192.168.2/24, class 12, priority 5. D: any
     * address, UDP, class 17, priority 1. */
    acl_entry(&up, P4_UPDATE_INSERT, IP4(192, 168, 0, 0), 0xFFFF0000u, 0, 0, 10,
              6);
    check(update(eng, &up) == 0, "ACL row A is not installed");
    acl_entry(&up, P4_UPDATE_INSERT, IP4(192, 168, 1, 0), 0xFFFFFF00u, 0, 0, 20,
              12);
    check(update(eng, &up) == 0, "ACL row B is not installed");
    acl_entry(&up, P4_UPDATE_INSERT, IP4(192, 168, 2, 0), 0xFFFFFF00u, 0, 0, 5,
              12);
    check(update(eng, &up) == 0, "ACL row C is not installed");
    acl_entry(&up, P4_UPDATE_INSERT, 0, 0, PROTO_UDP, 0xFFu, 1, 17);
    check(update(eng, &up) == 0, "ACL row D is not installed");

    ip_frame(&f, IP4(192, 168, 1, 5), IP4(10, 1, 2, 3), 64, PROTO_TCP);
    check(forwards_to(eng, &f, P4_PORT_UPLINK),
          "B, inserted after A, did not win on its higher priority");
    ip_frame(&f, IP4(192, 168, 2, 5), IP4(10, 1, 2, 3), 64, PROTO_TCP);
    check(drops(eng, &f), "C, inserted after A, won on a lower priority");
    ip_frame(&f, IP4(192, 168, 3, 5), IP4(10, 1, 2, 3), 64, PROTO_TCP);
    check(drops(eng, &f), "A alone did not set class 6");
    ip_frame(&f, IP4(10, 9, 9, 9), IP4(10, 1, 2, 3), 64, PROTO_UDP);
    check(drops(eng, &f), "D alone did not set class 17");
    ip_frame(&f, IP4(192, 168, 1, 5), IP4(10, 1, 2, 3), 64, PROTO_UDP);
    check(forwards_to(eng, &f, P4_PORT_UPLINK),
          "D, the lowest priority, beat B");

    /* The table holds 4. */
    acl_entry(&up, P4_UPDATE_INSERT, IP4(172, 16, 0, 0), 0xFFF00000u, 0, 0, 1,
              1);
    check(update(eng, &up) == -ENOSPC, "a fifth row went into a 4-row table");

    /* A bit outside the mask is malformed, as in P4Runtime, not another
     * way of writing A. */
    acl_entry(&up, P4_UPDATE_INSERT, IP4(192, 168, 7, 7), 0xFFFF0000u, 0, 0, 10,
              6);
    check(update(eng, &up) == -EINVAL, "a bit outside the mask was accepted");
    acl_entry(&up, P4_UPDATE_DELETE, IP4(192, 168, 7, 7), 0xFFFF0000u, 0, 0, 10,
              0);
    check(update(eng, &up) == -EINVAL,
          "a delete with a bit outside the mask was accepted");

    /* An entry in a table with a ternary key needs a nonzero priority, as
     * in P4Runtime. */
    acl_entry(&up, P4_UPDATE_INSERT, IP4(172, 16, 0, 0), 0xFFF00000u, 0, 0, 0,
              1);
    check(update(eng, &up) == -EINVAL,
          "a ternary insert with priority 0 was accepted");
    acl_entry(&up, P4_UPDATE_DELETE, IP4(192, 168, 0, 0), 0xFFFF0000u, 0, 0, 0,
              0);
    check(update(eng, &up) == -EINVAL,
          "a ternary delete with priority 0 was accepted");

    /* A ternary field takes a mask, not a prefix. */
    acl_entry(&up, P4_UPDATE_DELETE, IP4(192, 168, 0, 0), 0xFFFF0000u, 0, 0, 10,
              0);
    up.match[0].prefix_bits = 16;
    check(update(eng, &up) == -EINVAL,
          "a delete with a prefix on a ternary field was not refused");

    /* A's key at a higher priority is a row of its own. */
    acl_entry(&up, P4_UPDATE_DELETE, IP4(192, 168, 2, 0), 0xFFFFFF00u, 0, 0, 5,
              0);
    check(update(eng, &up) == 0, "ACL row C is not deleted");
    acl_entry(&up, P4_UPDATE_INSERT, IP4(192, 168, 0, 0), 0xFFFF0000u, 0, 0, 30,
              3);
    check(update(eng, &up) == 0,
          "A's key at another priority is not a row of its own");
    ip_frame(&f, IP4(192, 168, 3, 5), IP4(10, 1, 2, 3), 64, PROTO_TCP);
    check(forwards_to(eng, &f, P4_PORT_UPLINK),
          "A's key at priority 30 did not beat it at 10");

    /* A different mask is a different key, so there is nothing to delete. */
    acl_entry(&up, P4_UPDATE_DELETE, IP4(192, 168, 0, 0), 0xFFFFFF00u, 0, 0, 10,
              0);
    check(update(eng, &up) == -ENOENT,
          "a delete under a different mask found a row");

    p4_engine_destroy(eng);
}

/* The entry store and the executor, called the wrong way. */
static void test_entries_api(void)
{
    struct p4_program *prog = NULL;
    struct p4_entries *ent;
    struct p4_exec_result er;
    struct p4_rdma_meta meta;
    struct p4_update up;
    struct p4_value keys[P4_MAX_TABLE_KEY];
    char err[160];
    uint8_t buf[64];
    size_t n = 0;
    char *text = read_fixture("features.json", &n);

    if (!text) {
        check(false, "features.json could not be read");
        return;
    }
    checkf(p4_json_parse(text, n, &prog, err, sizeof(err)) == 0,
           "features.json is refused: %s", err);
    free(text);
    if (!prog)
        return;

    check(p4_entries_create(NULL) == NULL, "a store was made for no program");
    ent = p4_entries_create(prog);
    if (!ent) {
        check(false, "p4_entries_create failed");
        p4_json_free(prog);
        return;
    }

    route_entry(&up, P4_UPDATE_INSERT, IP4(10, 0, 0, 0), 8, 1);
    check(p4_entries_update(NULL, prog, &up, err, sizeof(err)) == -EINVAL,
          "an update with no store was accepted");
    up.type = 7;
    check(p4_entries_update(ent, prog, &up, err, sizeof(err)) == -EINVAL &&
              strstr(err, "unknown update type") != NULL,
          "an unknown update type was accepted");
    route_entry(&up, P4_UPDATE_INSERT, IP4(10, 0, 0, 0), 8, 1);
    up.nmatch = P4_MAX_TABLE_KEY + 1;
    check(p4_entries_update(ent, prog, &up, err, sizeof(err)) == -EINVAL &&
              strstr(err, "too large") != NULL,
          "an entry with too many key fields was accepted");

    route_entry(&up, P4_UPDATE_INSERT, IP4(10, 0, 0, 0), 8, 1);
    check(p4_entries_update(ent, prog, &up, err, sizeof(err)) == 0,
          "a route is not installed");

    memset(keys, 0, sizeof(keys));
    check(p4_entries_lookup(NULL, prog, 0, 0, keys) == NULL &&
              p4_entries_lookup(ent, NULL, 0, 0, keys) == NULL &&
              p4_entries_lookup(ent, prog, prog->npipelines, 0, keys) == NULL,
          "a lookup in nothing found something");

    memset(buf, 0, sizeof(buf));
    memset(&meta, 0, sizeof(meta));
    check(p4_exec_run(NULL, ent, 0, buf, sizeof(buf), buf, sizeof(buf), &er) ==
                  -EINVAL &&
              p4_exec_run(prog, ent, 0, buf, 0, buf, sizeof(buf), &er) ==
                  -EINVAL,
          "the executor ran with nothing to run");
    check(p4_exec_run_meta(NULL, ent, NULL, &meta, &er) == -EINVAL &&
              p4_exec_run_meta(prog, ent, NULL, NULL, &er) == -EINVAL,
          "the executor ran an operation with nothing to run");

    p4_entries_destroy(ent);
    p4_json_free(prog);
}

/* The engine's own entry points, called with nothing behind them. */
static void test_engine_edges(void)
{
    struct p4_engine *eng = p4_engine_create();
    struct p4_rdma_meta meta;
    struct p4_update up;
    bool suppress = true;
    char err[4];

    memset(&meta, 0, sizeof(meta));
    check(p4_engine_process_rdma(eng, NULL, &meta, &suppress) == -ENOENT &&
              !suppress,
          "an operation ran with no program loaded");

    memset(&up, 0, sizeof(up));
    check(p4_engine_update(NULL, &up, err, sizeof(err)) == -EINVAL,
          "an update with no engine was accepted");
    /* The reason does not fit, and is cut short rather than overrunning. */
    memset(err, 'x', sizeof(err));
    check(p4_engine_update(eng, &up, err, sizeof(err)) == -ENOENT &&
              memcmp(err, "no ", 4) == 0,
          "a short error buffer was not filled and terminated");

    check(p4_engine_load_chunk(NULL, 0, err, 1) == -EINVAL,
          "a chunk with no engine was accepted");
    check(p4_engine_load_commit(NULL) == -EINVAL,
          "a commit with no engine was accepted");

    p4_engine_destroy(eng);
}

int main(void)
{
    test_lifecycle();
    test_load_parses();
    test_load_real_artifacts();
    test_load_cost_budget();
    test_execute();
    test_execute_egress();
    test_execute_no_egress_assignment();
    test_execute_wide_field();
    test_table_entries();
    test_table_entry_errors();
    test_rdma_metadata();
    test_rdma_execute();
    test_rdma_few_fields();
    test_extern_call();
    test_extern_after_drop();
    test_reject_corpus();
    test_parser_loop();
    test_parser_extract_twice();
    test_duplicate_keys();
    test_deparse_overflow();
    test_features_parser();
    test_features_actions();
    test_features_lpm();
    test_lpm_key_not_first();
    test_features_ternary();
    test_entries_api();
    test_engine_edges();
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
