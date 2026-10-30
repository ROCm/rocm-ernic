/*
 * Unit tests for ionic_bar0_classify() in ionic/ionic_bar.h.
 *
 * The server's BAR0 callback sends an access to the ionic register window
 * or the MSI-X shadow above it by what this returns, and refuses anything
 * else. libvfio-user refuses an access outside the region before calling
 * the server, so a client can't reach the out-of-bounds cases; they are
 * here instead: a negative offset, and a count that would wrap when added
 * to the offset.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>

#include "ionic/ionic_bar.h"

#define REGS  IONIC_BAR0_REGS_SIZE
#define TOTAL IONIC_BAR0_TOTAL_SIZE

static const char *window_name(enum ionic_bar0_window w)
{
    static const char *const names[] = {
        [IONIC_BAR0_WINDOW_REGS] = "regs",
        [IONIC_BAR0_WINDOW_MSIX] = "msix",
        [IONIC_BAR0_WINDOW_OUTSIDE] = "outside",
        [IONIC_BAR0_WINDOW_STRADDLE] = "straddle",
    };

    if ((size_t)w >= sizeof(names) / sizeof(names[0])) {
        return "unknown";
    }
    return names[w];
}

int main(void)
{
    static const struct {
        const char *what;
        int64_t offset;
        size_t count;
        enum ionic_bar0_window want;
    } cases[] = {
        {"the signature", 0, 4, IONIC_BAR0_WINDOW_REGS},
        {"the whole register window", 0, REGS, IONIC_BAR0_WINDOW_REGS},
        {"the last register", REGS - 4, 4, IONIC_BAR0_WINDOW_REGS},
        {"the first MSI-X entry", REGS, 16, IONIC_BAR0_WINDOW_MSIX},
        {"all of MSI-X", REGS, TOTAL - REGS, IONIC_BAR0_WINDOW_MSIX},
        {"the last byte of BAR0", TOTAL - 1, 1, IONIC_BAR0_WINDOW_MSIX},
        {"across the split", REGS - 4, 8, IONIC_BAR0_WINDOW_STRADDLE},
        {"the whole of BAR0", 0, TOTAL, IONIC_BAR0_WINDOW_STRADDLE},
        {"one byte past the end", TOTAL - 4, 5, IONIC_BAR0_WINDOW_OUTSIDE},
        {"starting at the end", TOTAL, 1, IONIC_BAR0_WINDOW_OUTSIDE},
        {"starting past the end", TOTAL + 1, 0, IONIC_BAR0_WINDOW_OUTSIDE},
        {"more than BAR0 from 0", 0, TOTAL + 1, IONIC_BAR0_WINDOW_OUTSIDE},
        {"a negative offset", -1, 1, IONIC_BAR0_WINDOW_OUTSIDE},
        {"a negative offset ending inside", -4, 8, IONIC_BAR0_WINDOW_OUTSIDE},
        {"the most negative offset", INT64_MIN, 4, IONIC_BAR0_WINDOW_OUTSIDE},
        /* offset + count wraps to 4, inside the registers */
        {"a count that wraps", 8, SIZE_MAX - 3, IONIC_BAR0_WINDOW_OUTSIDE},
        {"a count that wraps from MSI-X", REGS, SIZE_MAX - REGS + 1,
         IONIC_BAR0_WINDOW_OUTSIDE},
        {"the largest count", 0, SIZE_MAX, IONIC_BAR0_WINDOW_OUTSIDE},
        {"the largest offset", INT64_MAX, 1, IONIC_BAR0_WINDOW_OUTSIDE},
    };
    int failures = 0;

    printf("ionic BAR0 window unit tests\n");

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        enum ionic_bar0_window got =
            ionic_bar0_classify(cases[i].offset, cases[i].count);

        if (got != cases[i].want) {
            printf("  FAIL: %s: offset=%" PRId64 " count=%zu gave %s, "
                   "wanted %s\n",
                   cases[i].what, cases[i].offset, cases[i].count,
                   window_name(got), window_name(cases[i].want));
            failures++;
        }
    }

    if (failures) {
        printf("ionic BAR0 window: %d case(s) FAILED\n", failures);
        return 1;
    }
    printf("ionic BAR0 window: all %zu cases passed\n",
           sizeof(cases) / sizeof(cases[0]));
    return 0;
}
