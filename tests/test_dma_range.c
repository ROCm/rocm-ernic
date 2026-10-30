/*
 * Unit tests for ernic_dma_range_ok() in dma_range.h.
 *
 * The check every guest DMA address and length has to pass before it
 * reaches vfu_addr_to_sgl(). libvfio-user's fast path compares
 * ``dma_addr + len <= iov_end(region)`` on uint64_t, so a length large
 * enough to wrap that addition turns the comparison into a test against a
 * small number, which a legitimately mapped region passes; the
 * translation that comes back then describes the full length. Unsigned
 * wrap-around is defined behaviour, so no sanitizer reports it, which is
 * why it is written out and tested here.
 *
 * Table-driven, and deliberately heavy on the boundary: the last byte of
 * the address space has to pass, and one past it has to fail.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "dma_range.h"

static int failures;

static void check(bool got, uint64_t gpa, uint64_t len, bool want,
                  const char *what)
{
    if (got == want) {
        return;
    }
    printf("  FAIL: %s: gpa=%#" PRIx64 " len=%#" PRIx64 " gave %s, wanted %s\n",
           what, gpa, len, got ? "ok" : "refused", want ? "ok" : "refused");
    failures++;
}

int main(void)
{
    static const struct {
        const char *what;
        uint64_t gpa;
        uint64_t len;
        bool want;
    } cases[] = {
        {"an ordinary range", 0x10000000, 4096, true},
        {"a zero length", 0x10000000, 0, true},
        {"a zero length at the very top", UINT64_MAX, 0, true},
        {"the whole address space", 0, UINT64_MAX, true},
        {"the last byte", UINT64_MAX, 1, false},
        {"the last byte, reached exactly", UINT64_MAX - 1, 1, true},
        {"one byte past the end", UINT64_MAX - 1, 2, false},
        {"a length that wraps to a small number", 0x10000000,
         UINT64_MAX - 0x1000, false},
        {"a length that wraps to zero", 1, UINT64_MAX, false},
        {"both halves large", UINT64_MAX / 2 + 1, UINT64_MAX / 2 + 1, false},
        {"both halves large, by one less", UINT64_MAX / 2 + 1, UINT64_MAX / 2,
         true},
    };

    printf("DMA range check unit tests\n");

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        check(ernic_dma_range_ok(cases[i].gpa, cases[i].len), cases[i].gpa,
              cases[i].len, cases[i].want, cases[i].what);
    }

    /*
     * Whatever the inputs, the check must agree with the arithmetic done
     * in a wider way: a range is fine exactly when its end does not pass
     * the top of the address space. Walked rather than asserted at a few
     * points, so a rewrite that gets an inequality backwards is caught.
     */
    for (uint64_t shift = 0; shift < 64; shift++) {
        uint64_t len = UINT64_C(1) << shift;
        uint64_t gpa = UINT64_MAX - len; /* the largest start that fits */

        check(ernic_dma_range_ok(gpa, len), gpa, len, true,
              "the largest start that fits");
        check(ernic_dma_range_ok(gpa + 1, len), gpa + 1, len, false,
              "one past the largest start that fits");
    }

    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
