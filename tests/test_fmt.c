/*
 * test_fmt.c - format what a test acts on, failing if it doesn't fit
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "test_fmt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void test_fmt(char *buf, size_t len, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= len) {
        fprintf(stderr, "test_fmt: \"%s\" doesn't fit in %zu bytes\n", fmt,
                len);
        abort();
    }
}
