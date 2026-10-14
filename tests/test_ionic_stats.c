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
 * dispatch_wqe() against a fake guest memory pre-filled with a poison byte,
 * and checks the completion status, how many bytes the handler wrote, and
 * what it wrote.  The admin queue is compiled separately with a test interface
 * to dispatch_wqe(), and its external symbols are stubbed.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <endian.h>
#include <syslog.h>
#include <sys/mman.h>
#include "ionic_adminq_internal.h"

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

    /* negative */
    {"negative", "QP values for an unmapped qp_id",
     "rc 1 and nothing written, not a page of zeros", QP_VALS, BUF_GPA,
     PAGE_LEN, (QP_ID + 100) | VER1, STATS_BODY_LEN, 0, 0, false, 1, 0,
     CONTENT_NONE},
    {"negative", "headers with dma_addr 0", "rc 1 and nothing written", HDRS, 0,
     PAGE_LEN, 0, STATS_BODY_LEN, 0, 0, false, 1, 0, CONTENT_NONE},
    {"negative", "headers with a 16-byte body (no type_state)",
     "rc 1 and nothing written", HDRS, BUF_GPA, PAGE_LEN, 0, STATS_BODY_LEN - 1,
     0, 0, false, 1, 0, CONTENT_NONE},
    {"negative", "device values when the counter snapshot fails",
     "rc 1 and nothing written", VALS, BUF_GPA, PAGE_LEN, 0, STATS_BODY_LEN,
     -22, 0, false, 1, 0, CONTENT_NONE},
    {"negative", "QP values when the counter snapshot fails",
     "rc 1 and nothing written", QP_VALS, BUF_GPA, PAGE_LEN, QP_ID | VER1,
     STATS_BODY_LEN, 0, -22, false, 1, 0, CONTENT_NONE},
    {"negative", "headers into a buffer that runs off guest memory",
     "rc 1 from the failed DMA and nothing written", HDRS, GUEST_MEM_SIZE - 64,
     PAGE_LEN, 0, STATS_BODY_LEN, 0, 0, false, 1, 0, CONTENT_NONE},

    /* boundary */
    {"boundary", "headers with length 31, one byte short of an entry",
     "rc 1 and nothing written", HDRS, BUF_GPA, 31, 0, STATS_BODY_LEN, 0, 0,
     false, 1, 0, CONTENT_NONE},
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
     "rc 1 and nothing written", VALS, BUF_GPA, 7, 0, STATS_BODY_LEN, 0, 0,
     false, 1, 0, CONTENT_NONE},
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

    rc = ionic_adminq_test_dispatch(&g_ctx, c->op, body, c->body_len);
    written = bytes_written(c->dma);
    ok = rc == c->want_rc && written == c->want_written &&
         one_write_or_none(c) && content_matches(c);

    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok) {
        printf("     got rc %u (want %u), %zu bytes written (want %zu), "
               "%zu DMA write(s)\n",
               rc, c->want_rc, written, c->want_written, g_nwrites);
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

int main(void)
{
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_case(&cases[i]);
    check_names("device stat", dev_stats, DEV_STATS_COUNT);
    check_names("QP stat", qp_stats, QP_STATS_COUNT);

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
