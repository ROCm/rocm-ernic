/*
 * libFuzzer harness for the ionic hardware statistics admin commands.
 *
 * STATS_HDRS, STATS_VALS, QP_STATS_HDRS and QP_STATS_VALS take a body the
 * guest driver controls -- a DMA address, a length and a QP id -- and
 * write a table into guest memory.  The harness posts one of the four
 * through the real ionic_adminq_dispatch_wqe() against the fake device in
 * tests/ionic_adminq_fakes.h, and checks every result against what the
 * handler promises:
 *
 *   - the status is 0 or 1, and a failed command writes nothing;
 *   - a successful one does exactly one DMA write, at the buffer the body
 *     named, no longer than its length, and in whole entries;
 *   - a header table that fits ends in the all-zero NONE terminator, and
 *     every entry is an LE64 at the next 8-byte slot with its name;
 *   - values are the counters the stubs returned, in header order;
 *   - QP values are only ever written for a mapped QP;
 *   - no byte of guest memory outside the write changes.
 *
 * A broken promise aborts, which libFuzzer reports as a crash.  ASan
 * catches a write that leaves guest memory altogether.
 *
 * Input layout:
 *   byte 0      bits 0-1 the opcode (one of the four above), bit 2 the
 *               device snapshot fails, bit 3 the QP snapshot fails, bit 4
 *               every counter is UINT64_MAX
 *   byte 1      the body length (mod 65)
 *   then        the body; missing bytes are zero
 *   then        the start of guest memory (the rest is a poison byte)
 *
 * LLVMFuzzerInitialize() first runs a table of seed inputs -- positive,
 * negative, boundary and corner -- through the same checks plus the
 * status each one expects, so the oracle itself is tested on every run;
 * a failing seed aborts before any fuzzing starts.
 * With FUZZ_EMIT_SEEDS=<dir> it also writes them there as a seed corpus.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <endian.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ionic_adminq_priv.h"

#include "ionic_adminq_fakes.h"

#include "fuzz_harness.h"

#define MAX_BODY   64
#define IN_HDR     ((size_t)2)
#define F_OP_MASK  0x03u
#define F_DEV_FAIL 0x04u
#define F_QP_FAIL  0x08u
#define F_MAX      0x10u
#define QP_ID_MASK 0x00ffffffu

static const uint8_t ops[4] = {
    IONIC_V1_ADMIN_STATS_HDRS,
    IONIC_V1_ADMIN_STATS_VALS,
    IONIC_V1_ADMIN_QP_STATS_HDRS,
    IONIC_V1_ADMIN_QP_STATS_VALS,
};

static uint8_t g_before[GUEST_MEM_SIZE];

static bool is_hdrs(uint8_t op)
{
    return op == IONIC_V1_ADMIN_STATS_HDRS ||
           op == IONIC_V1_ADMIN_QP_STATS_HDRS;
}

static bool is_qp(uint8_t op)
{
    return op == IONIC_V1_ADMIN_QP_STATS_HDRS ||
           op == IONIC_V1_ADMIN_QP_STATS_VALS;
}

static bool qp_is_mapped(uint32_t qp_id)
{
    return qp_id == QP_ID || qp_id == QP2_ID;
}

/* The header table at @w is @stats in order, then NONE if it fits. */
static const char *check_hdrs(const struct fake_sg *w, uint32_t length,
                              const struct adminq_stat *stats, size_t count)
{
    static const uint8_t zero[STAT_HDR_SIZE];
    size_t want = length / STAT_HDR_SIZE;

    if (want > count + 1)
        want = count + 1;
    if (w->len != want * STAT_HDR_SIZE)
        return "header write is not min(count + 1, length / 32) entries";

    for (size_t i = 0; i < want; i++) {
        const uint8_t *e = g_mem + w->addr + i * STAT_HDR_SIZE;
        uint32_t be;

        if (i == count) {
            if (memcmp(e, zero, sizeof(zero)) != 0)
                return "the NONE terminator is not all zero";
            continue;
        }
        memcpy(&be, e, 4);
        be = be32toh(be);
        if (be >> STAT_TYPE_SHIFT != STAT_TYPE_LE64 ||
            (be & 0x0fffffffu) != i * STAT_VAL_SIZE)
            return "a header is not an LE64 at the next 8-byte slot";
        if (memchr(e + 4, '\0', STAT_NAME_LEN) == NULL ||
            strcmp((const char *)e + 4, stats[i].name) != 0)
            return "a header name is unterminated or not the table's";
    }
    return NULL;
}

/* The values at @w are the stubbed counters in header order. */
static const char *check_vals(const struct fake_sg *w, uint32_t length,
                              const struct adminq_stat *stats, size_t count,
                              bool qp, uint32_t qp_id)
{
    size_t want = length / STAT_VAL_SIZE;

    if (want > count)
        want = count;
    if (w->len != want * STAT_VAL_SIZE)
        return "value write is not min(count, length / 8) values";

    for (size_t i = 0; i < want; i++) {
        size_t field = stats[i].off / sizeof(uint64_t);
        uint64_t v;
        uint64_t expect = qp ? qp_value(qp_id, field) : dev_value(field);

        memcpy(&v, g_mem + w->addr + i * STAT_VAL_SIZE, sizeof(v));
        if (le64toh(v) != expect)
            return "a value is not the counter its header names";
    }
    return NULL;
}

/*
 * Post one input and check every promise.  Returns NULL when they all
 * hold, else what broke; *rc_out is the completion status.
 */
static const char *run_input(const uint8_t *data, size_t size, uint8_t *rc_out)
{
    uint8_t body[MAX_BODY];
    uint8_t flags;
    uint8_t op;
    uint8_t rc;
    size_t body_len;
    uint64_t dma = 0;
    uint32_t length = 0;
    uint32_t id_ver = 0;
    const struct fake_sg *w = &g_writes[0];

    *rc_out = 0;
    if (size < IN_HDR)
        return NULL;
    flags = data[0];
    op = ops[flags & F_OP_MASK];
    body_len = data[1] % (MAX_BODY + 1);

    ctx_reset();
    g_dev_rc = (flags & F_DEV_FAIL) ? -1 : 0;
    g_qp_rc = (flags & F_QP_FAIL) ? -1 : 0;
    g_max = (flags & F_MAX) != 0;

    memset(body, 0, sizeof(body));
    if (size - IN_HDR < body_len)
        memcpy(body, data + IN_HDR, size - IN_HDR);
    else
        memcpy(body, data + IN_HDR, body_len);
    if (size > IN_HDR + body_len) {
        size_t mem_len = size - IN_HDR - body_len;

        if (mem_len > GUEST_MEM_SIZE)
            mem_len = GUEST_MEM_SIZE;
        memcpy(g_mem, data + IN_HDR + body_len, mem_len);
    }
    memcpy(g_before, g_mem, sizeof(g_before));

    if (body_len >= STATS_BODY_LEN) {
        memcpy(&dma, body, 8);
        dma = le64toh(dma);
        memcpy(&length, body + 8, 4);
        length = le32toh(length);
        memcpy(&id_ver, body + 12, 4);
        id_ver = le32toh(id_ver);
    }

    rc = ionic_adminq_dispatch_wqe(&g_ctx, op, body, (uint16_t)body_len);
    *rc_out = rc;

    if (rc != 0 && rc != 1)
        return "status is neither 0 nor 1";
    if (g_nwrites > 1)
        return "more than one DMA write";
    for (size_t i = 0; i < GUEST_MEM_SIZE; i++) {
        bool inside = g_nwrites == 1 && i >= w->addr && i < w->addr + w->len;

        if (!inside && g_mem[i] != g_before[i])
            return "a byte outside the write changed";
    }
    if (rc != 0)
        return g_nwrites == 0 ? NULL : "a failed command wrote";

    if (body_len < STATS_BODY_LEN)
        return "a short body succeeded";
    if (dma == 0)
        return "a zero dma_addr succeeded";
    if (g_nwrites != 1)
        return "success without a DMA write";
    if (w->addr != dma)
        return "wrote somewhere other than dma_addr";
    if (w->len > length)
        return "wrote past length";

    if (is_hdrs(op))
        return is_qp(op) ? check_hdrs(w, length, qp_stats, QP_STATS_COUNT)
                         : check_hdrs(w, length, dev_stats, DEV_STATS_COUNT);
    if (is_qp(op)) {
        if (g_qp_rc != 0)
            return "QP values succeeded though the snapshot failed";
        if (!qp_is_mapped(id_ver & QP_ID_MASK))
            return "QP values written for an unmapped QP";
        return check_vals(w, length, qp_stats, QP_STATS_COUNT, true,
                          id_ver & QP_ID_MASK);
    }
    if (g_dev_rc != 0)
        return "device values succeeded though the snapshot failed";
    return check_vals(w, length, dev_stats, DEV_STATS_COUNT, false, 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t rc;
    const char *why = run_input(data, size, &rc);

    if (why) {
        fprintf(stderr, "fuzz_ionic_stats: %s (status %u)\n", why, rc);
        abort();
    }
    return 0;
}

/* ---- Seeds ------------------------------------------------------------- */

struct seed {
    const char *category;
    const char *description;
    const char *expected;
    uint8_t flags; /* op index and F_* bits */
    uint8_t body_len;
    uint64_t dma;
    uint32_t length;
    uint32_t id_ver;
    uint8_t want_rc;
};

#define OP_HDRS    0u
#define OP_VALS    1u
#define OP_QP_HDRS 2u
#define OP_QP_VALS 3u

static const struct seed seeds[] = {
    {"positive", "device headers into a full page",
     "the whole table and its NONE terminator", OP_HDRS, STATS_BODY_LEN,
     BUF_GPA, PAGE_LEN, 0, 0},
    {"positive", "device values into a full page", "every device counter",
     OP_VALS, STATS_BODY_LEN, BUF_GPA, PAGE_LEN, 0, 0},
    {"positive", "QP headers into a full page",
     "the whole QP table and its NONE terminator", OP_QP_HDRS, STATS_BODY_LEN,
     BUF_GPA, PAGE_LEN, 0, 0},
    {"positive", "QP values for a mapped QP", "that QP's counters", OP_QP_VALS,
     STATS_BODY_LEN, BUF_GPA, PAGE_LEN, QP2_ID, 0},
    {"negative", "QP values for an unmapped QP", "status 1, nothing written",
     OP_QP_VALS, STATS_BODY_LEN, BUF_GPA, PAGE_LEN, 1234, 1},
    {"negative", "a zero dma_addr", "status 1, nothing written", OP_HDRS,
     STATS_BODY_LEN, 0, PAGE_LEN, 0, 1},
    {"negative", "a 16-byte body", "status 1, nothing written", OP_HDRS,
     STATS_BODY_LEN - 1, BUF_GPA, PAGE_LEN, 0, 1},
    {"negative", "device values when the snapshot fails",
     "status 1, nothing written", OP_VALS | F_DEV_FAIL, STATS_BODY_LEN, BUF_GPA,
     PAGE_LEN, 0, 1},
    {"negative", "a buffer that runs off guest memory",
     "status 1, nothing written", OP_HDRS, STATS_BODY_LEN, GUEST_MEM_SIZE - 8,
     PAGE_LEN, 0, 1},
    {"boundary", "headers with a length one short of an entry",
     "status 1, nothing written", OP_HDRS, STATS_BODY_LEN, BUF_GPA,
     STAT_HDR_SIZE - 1, 0, 1},
    {"boundary", "headers with room for the table but not the terminator",
     "every header, no terminator", OP_QP_HDRS, STATS_BODY_LEN, BUF_GPA,
     (uint32_t)(QP_STATS_COUNT *STAT_HDR_SIZE), 0, 0},
    {"boundary", "values with a length of exactly one value",
     "the first counter only", OP_VALS, STATS_BODY_LEN, BUF_GPA, STAT_VAL_SIZE,
     0, 0},
    {"corner", "a dma_addr whose end wraps past UINT64_MAX",
     "status 1, nothing written", OP_HDRS, STATS_BODY_LEN, UINT64_MAX - 15,
     PAGE_LEN, 0, 1},
    {"corner", "QP values with every id_ver version bit set",
     "the version byte is ignored: QP_ID's counters", OP_QP_VALS,
     STATS_BODY_LEN, BUF_GPA, PAGE_LEN, 0xff000000u | QP_ID, 0},
    {"corner", "device values with every counter at UINT64_MAX",
     "all ones round-trip", OP_VALS | F_MAX, STATS_BODY_LEN, BUF_GPA, PAGE_LEN,
     0, 0},
};

/* Encode @s in the input layout; returns the length. */
static size_t encode_seed(const struct seed *s, uint8_t *out)
{
    uint64_t dma = htole64(s->dma);
    uint32_t length = htole32(s->length);
    uint32_t id_ver = htole32(s->id_ver);

    memset(out, 0, IN_HDR + STATS_BODY_LEN);
    out[0] = s->flags;
    out[1] = s->body_len;
    memcpy(out + IN_HDR, &dma, 8);
    memcpy(out + IN_HDR + 8, &length, 4);
    memcpy(out + IN_HDR + 12, &id_ver, 4);
    return IN_HDR + s->body_len;
}

static void emit_seed(const char *dir, size_t i, const uint8_t *buf, size_t len)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/seed-%02zu", dir, i);
    f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, len, f) != len) {
        fprintf(stderr, "fuzz_ionic_stats: cannot write %s\n", path);
        exit(EXIT_FAILURE);
    }
    fclose(f);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    const char *dir = getenv("FUZZ_EMIT_SEEDS");
    int failures = 0;

    (void)argc;
    (void)argv;
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        const struct seed *s = &seeds[i];
        uint8_t buf[IN_HDR + MAX_BODY];
        size_t len = encode_seed(s, buf);
        uint8_t rc;
        const char *why = run_input(buf, len, &rc);
        bool ok = why == NULL && rc == s->want_rc;

        printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", s->category,
               s->description, s->expected);
        if (!ok) {
            printf("     status %u (want %u)%s%s\n", rc, s->want_rc,
                   why ? ": " : "", why ? why : "");
            failures++;
        }
        if (dir)
            emit_seed(dir, i, buf, len);
    }
    fflush(stdout);
    if (failures) {
        /* libFuzzer ignores this function's return value, so a broken
         * seed has to stop the run itself. */
        fprintf(stderr, "fuzz_ionic_stats: %d seed(s) failed\n", failures);
        abort();
    }
    return 0;
}
