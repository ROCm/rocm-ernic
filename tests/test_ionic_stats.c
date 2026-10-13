/*
 * Unit tests for the ionic emulator's hardware statistics admin commands.
 *
 * STATS_HDRS and QP_STATS_HDRS answer with a table of struct ionic_v1_stat
 * entries that the driver turns into its hw_counters files; STATS_VALS and
 * QP_STATS_VALS answer with the LE64 values at the offsets those headers
 * give.  While the four opcodes were stubs, the driver never saw a header
 * table at all, so a guest had no hw_counters directory to read.
 *
 * The tests are one table: each row posts one command through
 * ionic_adminq_dispatch_wqe() against a fake guest memory pre-filled with a
 * poison byte, and checks the completion status, how many bytes the handler
 * wrote, and what it wrote.  The test links ionic_adminq.c, reaches its
 * context and statistics tables through ionic_adminq_priv.h, and stubs its
 * external symbols.
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
#include <string.h>
#include <sys/uio.h>

#include <vfio-user/libvfio-user.h>

#include "ionic_adminq_priv.h"
#include "ionic_datapath.h"
#include "ionic_fault.h"
#include "ionic_status.h"
#include "rocm_ernic_compat.h"

#include "ionic_adminq_fakes.h"

/* ---- Test helpers ------------------------------------------------------ */

static int failures;

/* The 17-byte ionic_admin_stats_hdr body, in a buffer of @len bytes. */
static void build_body(uint8_t *body, size_t len, uint64_t dma, uint32_t length,
                       uint32_t id_ver)
{
    uint64_t dma_le = htole64(dma);
    uint32_t length_le = htole32(length), id_ver_le = htole32(id_ver);

    memset(body, 0, len);
    memcpy(body, &dma_le, 8);
    memcpy(body + 8, &length_le, 4);
    memcpy(body + 12, &id_ver_le, 4);
}

/* Bytes from @gpa up to and including the last one that is not POISON. */
static size_t bytes_written(uint64_t gpa)
{
    size_t n = 0;

    for (size_t i = 0; gpa + i < GUEST_MEM_SIZE; i++)
        if (g_mem[gpa + i] != POISON)
            n = i + 1;
    return n;
}

/*
 * The first @entries headers at @gpa are @stats in order, each an LE64 at
 * the next 8-byte slot with a NUL-padded name; an entry past the table is
 * the all-zero NONE terminator.
 */
static bool hdrs_match(uint64_t gpa, const struct adminq_stat *stats,
                       size_t count, size_t entries)
{
    for (size_t i = 0; i < entries; i++) {
        const uint8_t *e = g_mem + gpa + i * STAT_HDR_SIZE;
        char name[STAT_NAME_LEN];
        uint32_t be;

        memcpy(&be, e, 4);
        memcpy(name, e + 4, sizeof(name));
        if (i >= count) {
            static const uint8_t zero[STAT_HDR_SIZE];

            if (memcmp(e, zero, sizeof(zero)) != 0)
                return false;
            continue;
        }
        if (be32toh(be) >> STAT_TYPE_SHIFT != STAT_TYPE_LE64 ||
            (be32toh(be) & 0x0fffffffu) != i * STAT_VAL_SIZE)
            return false;
        if (name[STAT_NAME_LEN - 1] != '\0' || strcmp(name, stats[i].name) != 0)
            return false;
        for (size_t j = strlen(name); j < sizeof(name); j++)
            if (name[j] != '\0')
                return false;
    }
    return true;
}

/* The first @n LE64 values at @gpa are value(0) .. value(n - 1). */
static bool vals_match(uint64_t gpa, size_t n, uint64_t (*value)(size_t))
{
    for (size_t i = 0; i < n; i++) {
        uint64_t le;

        memcpy(&le, g_mem + gpa + i * STAT_VAL_SIZE, 8);
        if (le64toh(le) != value(i))
            return false;
    }
    return true;
}

static uint64_t qp_value_for_qp(size_t i)
{
    return qp_value(QP_ID, i);
}

static uint64_t qp_value_for_qp2(size_t i)
{
    return qp_value(QP2_ID, i);
}

/* ---- Tests ------------------------------------------------------------- */

enum content {
    CONTENT_NONE,     /* nothing beyond rc and bytes written */
    CONTENT_DEV_HDRS, /* dev_stats headers, then the terminator if it fit */
    CONTENT_QP_HDRS,  /* qp_stats headers, then the terminator if it fit */
    CONTENT_DEV_VALS, /* the device counters */
    CONTENT_QP_VALS,  /* QP_ID's counters */
    CONTENT_QP2_VALS, /* QP2_ID's counters */
};

struct stats_case {
    const char *category;
    const char *description;
    const char *expected;
    uint8_t op;
    uint64_t dma;
    uint32_t length;
    uint32_t id_ver;
    uint16_t body_len;
    int dev_rc; /* pvrdma_get_dev_counters() result */
    int qp_rc;  /* pvrdma_get_qp_counters() result */
    bool max;   /* every counter is UINT64_MAX */
    uint8_t want_rc;
    size_t want_written; /* bytes from dma; 0 = guest memory untouched */
    enum content content;
};

#define HDRS    IONIC_V1_ADMIN_STATS_HDRS
#define QP_HDRS IONIC_V1_ADMIN_QP_STATS_HDRS
#define VALS    IONIC_V1_ADMIN_STATS_VALS
#define QP_VALS IONIC_V1_ADMIN_QP_STATS_VALS
#define VER1    (1u << 24)

static const struct stats_case cases[] = {
    /* positive */
    {"positive", "device headers into a page",
     "5 LE64 headers then a zeroed NONE terminator (192 bytes)", HDRS, BUF_GPA,
     PAGE_LEN, 0, STATS_BODY_LEN, 0, 0, false, 0, (DEV_STATS_COUNT + 1) * 32,
     CONTENT_DEV_HDRS},
    {"positive", "QP headers into a page",
     "8 LE64 headers then a zeroed NONE terminator (288 bytes)", QP_HDRS,
     BUF_GPA, PAGE_LEN, 0, STATS_BODY_LEN, 0, 0, false, 0,
     (QP_STATS_COUNT + 1) * 32, CONTENT_QP_HDRS},
    {"positive", "device values into a page",
     "the 5 device counters as LE64 at the header offsets (40 bytes)", VALS,
     BUF_GPA, PAGE_LEN, 0, STATS_BODY_LEN, 0, 0, false, 0, DEV_STATS_COUNT * 8,
     CONTENT_DEV_VALS},
    {"positive", "QP values for the QP named by id_ver",
     "that QP's 8 counters as LE64 (64 bytes)", QP_VALS, BUF_GPA, PAGE_LEN,
     QP_ID | VER1, STATS_BODY_LEN, 0, 0, false, 0, QP_STATS_COUNT * 8,
     CONTENT_QP_VALS},
    {"positive", "QP values for a second mapped QP",
     "the second QP's counters, not the first's", QP_VALS, BUF_GPA, PAGE_LEN,
     QP2_ID | VER1, STATS_BODY_LEN, 0, 0, false, 0, QP_STATS_COUNT * 8,
     CONTENT_QP2_VALS},

    {"boundary", "device header block ends exactly at guest-memory end",
     "192 bytes written; prefix untouched", HDRS, GUEST_MEM_SIZE - 192, 192, 0,
     STATS_BODY_LEN, 0, 0, false, 0, 192, CONTENT_DEV_HDRS},
    {"negative", "header block exceeds guest memory by one byte",
     "EIO; entire guest memory untouched", HDRS, GUEST_MEM_SIZE - 191, 192, 0,
     STATS_BODY_LEN, 0, 0, false, IONIC_RC_EIO, 0, CONTENT_NONE},
    {"boundary", "device values end exactly at guest-memory end",
     "40 bytes written; prefix untouched", VALS, GUEST_MEM_SIZE - 40, 40, 0,
     STATS_BODY_LEN, 0, 0, false, 0, 40, CONTENT_DEV_VALS},
    {"negative", "values exceed guest memory by one byte",
     "EIO; entire guest memory untouched", VALS, GUEST_MEM_SIZE - 39, 40, 0,
     STATS_BODY_LEN, 0, 0, false, IONIC_RC_EIO, 0, CONTENT_NONE},
    {"corner", "header DMA address plus length overflows",
     "EIO; no wrapped write", HDRS, UINT64_MAX - 15, 192, 0, STATS_BODY_LEN, 0,
     0, false, IONIC_RC_EIO, 0, CONTENT_NONE},
    {"corner", "value DMA at maximum address", "EIO; no wrapped write", VALS,
     UINT64_MAX, 40, 0, STATS_BODY_LEN, 0, 0, false, IONIC_RC_EIO, 0,
     CONTENT_NONE},

    /* negative */
    {"negative", "QP values for an unmapped qp_id",
     "ENOENT (7) and nothing written, not a page of zeros", QP_VALS, BUF_GPA,
     PAGE_LEN, (QP_ID + 100) | VER1, STATS_BODY_LEN, 0, 0, false,
     IONIC_RC_ENOENT, 0, CONTENT_NONE},
    {"negative", "headers with dma_addr 0", "BAD_ADDR (17) and nothing written",
     HDRS, 0, PAGE_LEN, 0, STATS_BODY_LEN, 0, 0, false, IONIC_RC_BAD_ADDR, 0,
     CONTENT_NONE},
    {"negative", "headers with a 16-byte body (no type_state)",
     "EINVAL (14) and nothing written", HDRS, BUF_GPA, PAGE_LEN, 0,
     STATS_BODY_LEN - 1, 0, 0, false, IONIC_RC_EINVAL, 0, CONTENT_NONE},
    {"negative", "device values when the counter snapshot fails",
     "EIO (3) and nothing written", VALS, BUF_GPA, PAGE_LEN, 0, STATS_BODY_LEN,
     -22, 0, false, IONIC_RC_EIO, 0, CONTENT_NONE},
    {"negative", "QP values when the counter snapshot fails",
     "EIO (3) and nothing written", QP_VALS, BUF_GPA, PAGE_LEN, QP_ID | VER1,
     STATS_BODY_LEN, 0, -22, false, IONIC_RC_EIO, 0, CONTENT_NONE},
    {"negative", "headers into a buffer that runs off guest memory",
     "EIO (3) from the failed DMA and nothing written", HDRS,
     GUEST_MEM_SIZE - 64, PAGE_LEN, 0, STATS_BODY_LEN, 0, 0, false,
     IONIC_RC_EIO, 0, CONTENT_NONE},

    /* boundary */
    {"boundary", "headers with length 31, one byte short of an entry",
     "ENOSPC (15) and nothing written", HDRS, BUF_GPA, 31, 0, STATS_BODY_LEN, 0,
     0, false, IONIC_RC_ENOSPC, 0, CONTENT_NONE},
    {"boundary", "headers with length 32, exactly one entry",
     "only the first header (32 bytes)", HDRS, BUF_GPA, 32, 0, STATS_BODY_LEN,
     0, 0, false, 0, 32, CONTENT_DEV_HDRS},
    {"boundary", "headers with room for the table but not the terminator",
     "5 headers and no terminator (160 bytes)", HDRS, BUF_GPA,
     DEV_STATS_COUNT * 32, 0, STATS_BODY_LEN, 0, 0, false, 0,
     DEV_STATS_COUNT * 32, CONTENT_DEV_HDRS},
    {"boundary", "headers with length one byte short of the terminator",
     "whole entries only: 5 headers (160 bytes)", HDRS, BUF_GPA,
     (DEV_STATS_COUNT + 1) * 32 - 1, 0, STATS_BODY_LEN, 0, 0, false, 0,
     DEV_STATS_COUNT * 32, CONTENT_DEV_HDRS},
    {"boundary", "values with length 7, one byte short of a value",
     "ENOSPC (15) and nothing written", VALS, BUF_GPA, 7, 0, STATS_BODY_LEN, 0,
     0, false, IONIC_RC_ENOSPC, 0, CONTENT_NONE},
    {"boundary", "values with length 8, exactly one value",
     "only the first value (8 bytes)", VALS, BUF_GPA, 8, 0, STATS_BODY_LEN, 0,
     0, false, 0, 8, CONTENT_DEV_VALS},
    {"boundary", "values with length 20, not a multiple of 8",
     "whole values only: 2 values (16 bytes)", VALS, BUF_GPA, 20, 0,
     STATS_BODY_LEN, 0, 0, false, 0, 16, CONTENT_DEV_VALS},
    {"boundary", "headers with a body of exactly 17 bytes",
     "accepted: the full table (192 bytes)", HDRS, BUF_GPA, PAGE_LEN, 0,
     STATS_BODY_LEN, 0, 0, false, 0, (DEV_STATS_COUNT + 1) * 32,
     CONTENT_DEV_HDRS},

    /* corner */
    {"corner", "QP values with every id_ver version bit set",
     "the version byte is ignored and QP_ID is still found", QP_VALS, BUF_GPA,
     PAGE_LEN, QP_ID | 0xff000000u, STATS_BODY_LEN, 0, 0, false, 0,
     QP_STATS_COUNT * 8, CONTENT_QP_VALS},
    {"corner", "device values with every counter at UINT64_MAX",
     "every value round-trips as all ones (40 bytes)", VALS, BUF_GPA, PAGE_LEN,
     0, STATS_BODY_LEN, 0, 0, true, 0, DEV_STATS_COUNT * 8, CONTENT_DEV_VALS},
    {"corner", "headers with a body longer than 17 bytes",
     "the trailing bytes are ignored: the full table", HDRS, BUF_GPA, PAGE_LEN,
     0, 64, 0, 0, false, 0, (DEV_STATS_COUNT + 1) * 32, CONTENT_DEV_HDRS},
    {"corner", "device headers ignore id_ver",
     "the full device table, as with id_ver 0", HDRS, BUF_GPA, PAGE_LEN,
     QP_ID | VER1, STATS_BODY_LEN, 0, 0, false, 0, (DEV_STATS_COUNT + 1) * 32,
     CONTENT_DEV_HDRS},
};

static bool content_matches(const struct stats_case *c)
{
    switch (c->content) {
    case CONTENT_NONE:
        return true;
    case CONTENT_DEV_HDRS:
        return hdrs_match(c->dma, dev_stats, DEV_STATS_COUNT,
                          c->want_written / STAT_HDR_SIZE);
    case CONTENT_QP_HDRS:
        return hdrs_match(c->dma, qp_stats, QP_STATS_COUNT,
                          c->want_written / STAT_HDR_SIZE);
    case CONTENT_DEV_VALS:
        return vals_match(c->dma, c->want_written / 8, dev_value);
    case CONTENT_QP_VALS:
        return g_counters_qp_id == QP_ID &&
               vals_match(c->dma, c->want_written / 8, qp_value_for_qp);
    case CONTENT_QP2_VALS:
        return g_counters_qp_id == QP2_ID &&
               vals_match(c->dma, c->want_written / 8, qp_value_for_qp2);
    default:
        return false;
    }
}

/*
 * The fake DMA layer saw exactly the write the row expects: none when
 * nothing should be written, otherwise one, at the buffer the command
 * named and of exactly the expected size.  The poison scan alone can't
 * tell a short write from a trailing byte that happens to equal POISON.
 */
static bool one_write_or_none(const struct stats_case *c)
{
    if (c->want_written == 0)
        return g_nwrites == 0;
    return g_nwrites == 1 && g_writes[0].addr == c->dma &&
           g_writes[0].len == c->want_written;
}

static void run_case(const struct stats_case *c)
{
    uint8_t body[64];
    uint8_t rc;
    size_t written;
    bool ok;

    ctx_reset();
    g_dev_rc = c->dev_rc;
    g_qp_rc = c->qp_rc;
    g_max = c->max;
    build_body(body, sizeof(body), c->dma, c->length, c->id_ver);

    rc = ionic_adminq_dispatch_wqe(&g_ctx, c->op, body, c->body_len);
    written = bytes_written(c->dma);
    ok = rc == c->want_rc && written == c->want_written &&
         one_write_or_none(c) && content_matches(c);
    for (size_t i = 0; i < sizeof(g_mem); i++)
        if ((uint64_t)i < c->dma || (uint64_t)i - c->dma >= c->want_written)
            ok = ok && g_mem[i] == POISON;

    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok) {
        printf("     got rc %u (want %u), %zu bytes written (want %zu), "
               "%zu DMA write(s)\n",
               rc, c->want_rc, written, c->want_written, g_nwrites);
        failures++;
    }
}

/* ---- Injected faults (ionic_fault.h) ---------------------------------- */

#define FAULT_MAX_CMDS 4

/*
 * One fault and a run of identical commands against it.  Each command must
 * complete with its own status; one that succeeds writes the full reply
 * (@want_written bytes) and one that fails writes nothing.
 */
struct fault_case {
    const char *category;
    const char *description;
    const char *expected;
    const char *spec; /* --inject string */
    uint8_t op;
    uint32_t id_ver;
    size_t ncmds;
    uint8_t want_rc[FAULT_MAX_CMDS];
    size_t want_written; /* bytes from a command that succeeds */
};

static const struct fault_case fault_cases[] = {
    {"positive",
     "device values with 'stats_vals: every 1 effect fail'",
     "EIO (3) and no bytes written",
     "stats_vals: every 1 effect fail",
     VALS,
     0,
     1,
     {IONIC_RC_EIO},
     DEV_STATS_COUNT * 8},
    {"positive",
     "device values with 'fail status eversion'",
     "status 1 (EVERSION) and no bytes written",
     "stats_vals: every 1 effect fail status eversion",
     VALS,
     0,
     1,
     {IONIC_RC_EVERSION},
     DEV_STATS_COUNT * 8},
    {"positive",
     "device values with 'fail status enoent'",
     "status 7 (ENOENT) and no bytes written",
     "stats_vals: every 1 effect fail status enoent",
     VALS,
     0,
     1,
     {IONIC_RC_ENOENT},
     DEV_STATS_COUNT * 8},
    {"boundary",
     "'after 2' over 3 device-values commands",
     "commands 1 and 3 succeed with the values; command 2 fails with EIO",
     "stats_vals: after 2 effect fail",
     VALS,
     0,
     3,
     {IONIC_RC_SUCCESS, IONIC_RC_EIO, IONIC_RC_SUCCESS},
     DEV_STATS_COUNT * 8},
    {"boundary",
     "'every 2' over 4 device-values commands",
     "commands 2 and 4 fail; 1 and 3 succeed",
     "stats_vals: every 2 effect fail",
     VALS,
     0,
     4,
     {IONIC_RC_SUCCESS, IONIC_RC_EIO, IONIC_RC_SUCCESS, IONIC_RC_EIO},
     DEV_STATS_COUNT * 8},
    {"corner",
     "device headers with a stats_vals fault",
     "headers still served: the site is STATS_VALS only",
     "stats_vals: every 1 effect fail",
     HDRS,
     0,
     2,
     {IONIC_RC_SUCCESS, IONIC_RC_SUCCESS},
     (DEV_STATS_COUNT + 1) * 32},
    {"corner",
     "QP values with a stats_vals fault",
     "not affected: QP values are the qp_stats_vals site",
     "stats_vals: every 1 effect fail",
     QP_VALS,
     QP_ID | VER1,
     2,
     {IONIC_RC_SUCCESS, IONIC_RC_SUCCESS},
     QP_STATS_COUNT * 8},
};

static void run_fault_case(const struct fault_case *c)
{
    struct ionic_fault_set *set = ionic_fault_set_new();
    char err[128] = "";
    uint8_t body[64];
    uint8_t got[FAULT_MAX_CMDS] = {0};
    size_t got_written[FAULT_MAX_CMDS] = {0};
    bool added =
        set && ionic_fault_set_add(set, c->spec, err, sizeof(err)) == 0;
    bool ok = added;

    ctx_reset();
    g_ctx.faults = set;
    build_body(body, sizeof(body), BUF_GPA, PAGE_LEN, c->id_ver);
    for (size_t k = 0; added && k < c->ncmds; k++) {
        size_t want = c->want_rc[k] == IONIC_RC_SUCCESS ? c->want_written : 0;

        memset(g_mem, POISON, sizeof(g_mem));
        g_nwrites = 0;
        got[k] = ionic_adminq_dispatch_wqe(&g_ctx, c->op, body, STATS_BODY_LEN);
        got_written[k] = bytes_written(BUF_GPA);
        if (got[k] != c->want_rc[k] || got_written[k] != want)
            ok = false;
    }
    g_ctx.faults = NULL;
    ionic_fault_set_free(set);

    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok) {
        if (err[0])
            printf("     spec rejected: %s\n", err);
        for (size_t k = 0; k < c->ncmds; k++)
            printf("     command %zu: got rc %u (want %u), %zu bytes written\n",
                   k + 1, got[k], c->want_rc[k], got_written[k]);
        failures++;
    }
}

/*
 * Corner: the driver forces name[27] to NUL, so a name of 28 or more bytes
 * would lose its tail and could collide with another.  Every name must fit
 * in 27 bytes and be unique within its table.
 */
static void check_names(const char *table, const struct adminq_stat *stats,
                        size_t count)
{
    bool ok = true;

    for (size_t i = 0; i < count; i++) {
        if (strlen(stats[i].name) > STAT_NAME_LEN - 1)
            ok = false;
        for (size_t j = 0; j < i; j++)
            if (strcmp(stats[i].name, stats[j].name) == 0)
                ok = false;
    }
    printf("%s corner: every %s name; expected: at most 27 bytes and unique\n",
           ok ? "ok  " : "FAIL", table);
    if (!ok)
        failures++;
}

/* Literal ABI names and bytes: never derive the oracle from dev_stats/qp_stats.
 */
static void check_golden_headers(void)
{
    static const char device[][28] = {
        "commands", "total_bytes_sent", "total_bytes_received",
        "total_bytes_rdma_read", "total_bytes_rdma_write"};
    static const char qp[][28] = {"doorbell_send",   "doorbell_recv",
                                  "wqes_processed",  "cqes_posted",
                                  "bytes_sent",      "bytes_received",
                                  "bytes_rdma_read", "bytes_rdma_write"};
    static const struct {
        const char *description;
        uint8_t opcode;
        const char (*names)[28];
        size_t count;
    } wire_cases[] = {{"device wire headers", HDRS, device, 5},
                      {"QP wire headers", QP_HDRS, qp, 8}};
    for (size_t row = 0; row < sizeof(wire_cases) / sizeof(wire_cases[0]);
         row++) {
        uint8_t body[64];
        bool ok;

        ctx_reset();
        build_body(body, sizeof(body), BUF_GPA, 4096, 0);
        ok = ionic_adminq_dispatch_wqe(&g_ctx, wire_cases[row].opcode, body,
                                       17) == 0;
        for (size_t i = 0; i <= wire_cases[row].count; i++) {
            uint8_t expected[32] = {0};

            if (i < wire_cases[row].count) {
                expected[0] = 0x40; /* big-endian type LE64, offset below 256 */
                expected[3] = (uint8_t)(i * 8);
                memcpy(expected + 4, wire_cases[row].names[i], 28);
            }
            ok = ok && memcmp(g_mem + BUF_GPA + i * 32, expected, 32) == 0;
        }
        printf("%s positive: %s; expected: literal ABI names/type/offset, zero "
               "padding and terminator\n",
               ok ? "ok" : "FAIL", wire_cases[row].description);
        if (!ok)
            failures++;
    }
}

static void check_alternating_qps(void)
{
    static const struct {
        const char *description;
        uint32_t id;
        uint8_t rc;
        uint64_t first;
    } qp_cases[] = {{"positive: first QP", QP_ID, 0, 0x701},
                    {"positive: second QP", QP2_ID, 0, 0x901},
                    {"negative: missing QP", 100, IONIC_RC_ENOENT, 0},
                    {"corner: first QP again after failure", QP_ID, 0, 0x701},
                    {"corner: second QP again", QP2_ID, 0, 0x901}};

    ctx_reset();
    for (size_t row = 0; row < sizeof(qp_cases) / sizeof(qp_cases[0]); row++) {
        uint8_t body[64], before[64];
        uint64_t first;
        uint8_t rc;
        bool ok;

        memcpy(before, g_mem + BUF_GPA, sizeof(before));
        build_body(body, sizeof(body), BUF_GPA, 64, qp_cases[row].id | VER1);
        rc = ionic_adminq_dispatch_wqe(&g_ctx, QP_VALS, body, 17);
        memcpy(&first, g_mem + BUF_GPA, 8);
        ok = rc == qp_cases[row].rc &&
             (rc ? memcmp(before, g_mem + BUF_GPA, sizeof(before)) == 0
                 : le64toh(first) == qp_cases[row].first);
        printf("%s %s; expected: rc %u, own first value %#llx or unchanged "
               "buffer on failure\n",
               ok ? "ok" : "FAIL", qp_cases[row].description, qp_cases[row].rc,
               (unsigned long long)qp_cases[row].first);
        if (!ok)
            failures++;
    }
}

int main(void)
{
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_case(&cases[i]);
    for (size_t i = 0; i < sizeof(fault_cases) / sizeof(fault_cases[0]); i++)
        run_fault_case(&fault_cases[i]);
    check_golden_headers();
    check_alternating_qps();
    check_names("device stat", dev_stats, DEV_STATS_COUNT);
    check_names("QP stat", qp_stats, QP_STATS_COUNT);

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
