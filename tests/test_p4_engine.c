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
#include <stdlib.h>
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
#define P4_HDRS                                             \
    "{\"__meta__\":{\"version\":[2,24]},"                   \
    "\"header_types\":[{\"name\":\"eth_t\",\"id\":0,"       \
    "\"fields\":[[\"dst\",48],[\"src\",48],[\"ty\",16]]}]," \
    "\"headers\":[{\"name\":\"eth\",\"id\":0,"              \
    "\"header_type\":\"eth_t\",\"metadata\":false}],"

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

#define P4_SKELETON                                      \
    P4_PARSER "\"deparsers\":[{\"name\":\"d\",\"id\":0," \
              "\"order\":[\"eth\"]}]}"

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

    /* ...but mentioning it empty is fine, which is how p4c emits it for a
     * program that does not use one. The walk then stops at the first
     * section this document is missing. */
    check(load_text(eng, "{\"__meta__\":{\"version\":[2,24]},\"checksums\":[],"
                         "\"header_types\":[],\"headers\":[]}") == -EINVAL,
          "an empty checksums list is accepted");
    check(strstr(p4_engine_load_error(eng), "parsers") != NULL,
          "the walk got past the feature check, to the missing parser");

    /* Header types and their fields, with a parser and deparser so the
     * walk reaches the part that is not written yet. */
    {
        check(load_text(eng, P4_SKELETON) == -ENOSYS,
              "a well-formed program is walked to the end of what exists");
        check(strstr(p4_engine_load_error(eng), "not finished") != NULL,
              "the walk got past the parser and deparser");
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
    check(load_fixture(eng, "loopback.json") == -ENOSYS,
          "bmv2's own loopback.json is walked, not rejected");
    why = p4_engine_load_error(eng);
    check(why != NULL && strstr(why, "not finished") != NULL,
          "loopback.json stopped only where the walk is unfinished");

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
    check(load_fixture(eng, "minimal_forward.json") == -ENOSYS,
          "the minimal forwarding program is walked");
    why = p4_engine_load_error(eng);
    check(why != NULL && strstr(why, "not finished") != NULL,
          "minimal_forward.json stopped only where the walk is unfinished");

    p4_engine_destroy(eng);
}

int main(void)
{
    test_lifecycle();
    test_load_parses();
    test_load_real_artifacts();
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
