/*
 * Unit tests for ernic_dma.c, the one path between host buffers and guest
 * memory.
 *
 * Runs against fake_vfu.c, whose guest memory is a row of small regions,
 * so a transfer can be made to cross one region boundary, or so many that
 * it needs more scatter/gather entries than ERNIC_DMA_SGL_MAX. It also
 * fails chosen libvfio-user calls, so each error path is run and checked
 * for mappings it didn't put back.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vfio-user/libvfio-user.h>

#include "ernic_dma.h"
#include "fake_vfu.h"

/* Small regions, so a few kilobytes cross more of them than one
 * transfer's list has room for. */
#define REGION_SIZE 64u
#define NREGIONS    512u
#define MEM_SIZE    ((uint64_t)REGION_SIZE * NREGIONS)

/* A transfer that needs about 200 entries, and its halves about 100. */
#define BIG_LEN (REGION_SIZE * 200u)
_Static_assert(BIG_LEN / REGION_SIZE > ERNIC_DMA_SGL_MAX,
               "BIG_LEN must need more entries than one list holds");
_Static_assert(BIG_LEN / 2 / REGION_SIZE + 2 <= ERNIC_DMA_SGL_MAX,
               "each half of BIG_LEN must fit in one list");

static int failures;

static vfu_ctx_t *g_vfu;
static struct ernic_dma *g_dma;

static void check(bool ok, const char *what)
{
    if (ok)
        return;
    printf("  FAIL: %s\n", what);
    failures++;
}

static void fill(uint8_t *buf, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed + i * 7u);
}

/* Nothing got with vfu_sgl_get() is still out. */
static void check_all_put(const char *what)
{
    if (fake_vfu_mapped(g_vfu) == 0)
        return;
    printf("  FAIL: %s: %zu entries left mapped\n", what,
           fake_vfu_mapped(g_vfu));
    failures++;
}

/*
 * Write @len bytes at @gpa and read them back, checking each against what
 * the fake's own copy routines see in guest memory. Returns how many
 * vfu_addr_to_sgl() calls the write took.
 */
static unsigned long round_trip(uint64_t gpa, size_t len, uint8_t seed,
                                const char *what)
{
    static uint8_t want[BIG_LEN], got[BIG_LEN];
    unsigned long before = fake_vfu_map_count(g_vfu);
    unsigned long calls;

    fill(want, len, seed);
    if (ernic_dma_write(g_dma, g_vfu, gpa, want, len) != 0) {
        printf("  FAIL: %s: write failed\n", what);
        failures++;
        return 0;
    }
    calls = fake_vfu_map_count(g_vfu) - before;
    check_all_put(what);

    memset(got, 0, len);
    check(fake_vfu_read(g_vfu, gpa, got, len) && memcmp(got, want, len) == 0,
          what);

    memset(got, 0, len);
    if (ernic_dma_read(g_dma, g_vfu, gpa, got, len) != 0) {
        printf("  FAIL: %s: read failed\n", what);
        failures++;
        return calls;
    }
    check(memcmp(got, want, len) == 0, what);
    check_all_put(what);
    return calls;
}

static void test_transfers(void)
{
    unsigned long calls;

    calls = round_trip(FAKE_VFU_GPA_BASE + 8, 40, 1, "inside one region");
    check(calls == 1, "inside one region: not one mapping");

    /* 40 + 64 + 16 bytes: three regions, one list. Every helper but the
     * data path's used to map exactly one entry, and refused this. */
    calls = round_trip(FAKE_VFU_GPA_BASE + REGION_SIZE + 24,
                       40 + REGION_SIZE + 16, 2, "across region boundaries");
    check(calls == 1, "across region boundaries: not one mapping");

    /* Too many entries for one list: the library answers -(needed) - 1
     * with errno 0, and the transfer is split. */
    calls = round_trip(FAKE_VFU_GPA_BASE + 5, BIG_LEN, 3,
                       "more regions than one list holds");
    check(calls == 3, "more regions than one list holds: not split in two");

    /* The whole of guest memory, which splits again below the halves: 512
     * regions take the whole, two halves of 256, then four quarters of 128,
     * each of which fits. A split piece's halves are each tried once, not
     * the rest of the transfer after every piece. */
    static uint8_t all[MEM_SIZE];
    unsigned long before = fake_vfu_map_count(g_vfu);
    fill(all, sizeof(all), 4);
    check(ernic_dma_write(g_dma, g_vfu, FAKE_VFU_GPA_BASE, all, sizeof(all)) ==
              0,
          "all of guest memory: write failed");
    check(fake_vfu_map_count(g_vfu) - before == 7,
          "all of guest memory: not split into quarters");
    static uint8_t back[MEM_SIZE];
    check(fake_vfu_read(g_vfu, FAKE_VFU_GPA_BASE, back, sizeof(back)) &&
              memcmp(back, all, sizeof(all)) == 0,
          "all of guest memory: wrong contents");
    check_all_put("all of guest memory");
}

static void test_zero_length(void)
{
    unsigned long before = fake_vfu_map_count(g_vfu);
    uint8_t b = 0x5a;

    /* Not even mapped: a zero length asks for nothing, wherever it is. */
    check(ernic_dma_read(g_dma, g_vfu, 0, &b, 0) == 0,
          "zero-length read failed");
    check(ernic_dma_write(g_dma, g_vfu, 0, &b, 0) == 0,
          "zero-length write failed");
    check(b == 0x5a, "zero-length read changed the buffer");
    check(fake_vfu_map_count(g_vfu) == before, "zero length was mapped");
}

static void test_refused(void)
{
    static const struct {
        const char *what;
        uint64_t gpa;
        uint64_t len;
        int want;
    } cases[] = {
        {"a range that wraps", UINT64_MAX - 10, 100, -EINVAL},
        {"a length that wraps to a small end", FAKE_VFU_GPA_BASE,
         UINT64_MAX - FAKE_VFU_GPA_BASE + 32, -EINVAL},
        {"below guest memory", FAKE_VFU_GPA_BASE - 4, 4, -EIO},
        {"straddling the start of guest memory", FAKE_VFU_GPA_BASE - 2, 4,
         -EIO},
        {"straddling the end of guest memory", FAKE_VFU_GPA_BASE + MEM_SIZE - 2,
         4, -EIO},
        {"past the end of guest memory", FAKE_VFU_GPA_BASE + MEM_SIZE, 1, -EIO},
    };
    static uint8_t buf[64];

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned long before = fake_vfu_map_count(g_vfu);
        /* Only the first bytes are ever touched: everything here is
         * refused before or at the mapping. */
        size_t len = (size_t)cases[i].len;
        int rr, wr;

        memset(buf, 0xee, sizeof(buf));
        rr = ernic_dma_read(g_dma, g_vfu, cases[i].gpa, buf, len);
        wr = ernic_dma_write(g_dma, g_vfu, cases[i].gpa, buf, len);
        if (rr != cases[i].want || wr != cases[i].want) {
            printf("  FAIL: %s: read gave %d, write %d, wanted %d\n",
                   cases[i].what, rr, wr, cases[i].want);
            failures++;
        }
        if (cases[i].want == -EINVAL && fake_vfu_map_count(g_vfu) != before) {
            printf("  FAIL: %s: reached vfu_addr_to_sgl()\n", cases[i].what);
            failures++;
        }
        check(buf[0] == 0xee, cases[i].what);
        check_all_put(cases[i].what);
    }
}

static void test_library_failures(void)
{
    uint8_t buf[100];
    uint8_t guest[sizeof(buf)];
    uint64_t gpa = FAKE_VFU_GPA_BASE + REGION_SIZE - 10;

    fill(guest, sizeof(guest), 9);
    fake_vfu_write(g_vfu, gpa, guest, sizeof(guest));

    fake_vfu_fail_nth_map(g_vfu, 1);
    memset(buf, 0, sizeof(buf));
    check(ernic_dma_read(g_dma, g_vfu, gpa, buf, sizeof(buf)) == -EIO,
          "a failed mapping didn't fail the read");
    check(buf[0] == 0, "a failed mapping filled the buffer");
    check_all_put("a failed mapping");

    fake_vfu_fail_nth_get(g_vfu, 1);
    check(ernic_dma_read(g_dma, g_vfu, gpa, buf, sizeof(buf)) == -EIO,
          "a failed vfu_sgl_get() didn't fail the read");
    check(buf[0] == 0, "a failed vfu_sgl_get() filled the buffer");
    check_all_put("a failed vfu_sgl_get()");

    fake_vfu_fail_nth_get(g_vfu, 1);
    fill(buf, sizeof(buf), 0x40);
    check(ernic_dma_write(g_dma, g_vfu, gpa, buf, sizeof(buf)) == -EIO,
          "a failed vfu_sgl_get() didn't fail the write");
    uint8_t now[sizeof(buf)];
    check(fake_vfu_read(g_vfu, gpa, now, sizeof(now)) &&
              memcmp(now, guest, sizeof(now)) == 0,
          "a failed vfu_sgl_get() wrote guest memory");
    check_all_put("a failed vfu_sgl_get() on a write");

    /* A split whose second half can't be mapped: calls are the whole (too
     * many entries), the first half, then the second. */
    static uint8_t big[BIG_LEN];
    fake_vfu_fail_nth_map(g_vfu, 3);
    check(ernic_dma_read(g_dma, g_vfu, FAKE_VFU_GPA_BASE + 5, big,
                         sizeof(big)) == -EIO,
          "a failed second half didn't fail the read");
    check_all_put("a failed second half");

    /* And one whose first half fails stops there. */
    unsigned long before = fake_vfu_map_count(g_vfu);
    fake_vfu_fail_nth_map(g_vfu, 2);
    check(ernic_dma_read(g_dma, g_vfu, FAKE_VFU_GPA_BASE + 5, big,
                         sizeof(big)) == -EIO,
          "a failed first half didn't fail the read");
    check(fake_vfu_map_count(g_vfu) - before == 2,
          "the second half was tried after the first failed");
    check_all_put("a failed first half");
}

int main(void)
{
    printf("Guest DMA unit tests\n");

    ernic_dma_destroy(NULL);

    g_vfu = fake_vfu_create(REGION_SIZE, NREGIONS);
    g_dma = ernic_dma_create();
    if (!g_vfu || !g_dma) {
        printf("FAILED: setup\n");
        ernic_dma_destroy(g_dma);
        fake_vfu_destroy(g_vfu);
        return 1;
    }

    test_transfers();
    test_zero_length();
    test_refused();
    test_library_failures();

    ernic_dma_destroy(g_dma);
    size_t leaked = fake_vfu_destroy(g_vfu);
    check(leaked == 0, "mappings were never put back");

    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
