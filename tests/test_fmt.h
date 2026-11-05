/*
 * test_fmt.h - format what a test acts on, failing if it doesn't fit
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef TEST_FMT_H
#define TEST_FMT_H

#include <stddef.h>

#include "ernic_attributes.h"

/**
 * test_fmt() - snprintf() into @p buf, aborting if the result is cut short
 *
 * For a string the test acts on, such as a path, a request, an option
 * string, or an expected value, where a truncated one would test
 * something else. A failure message, which may be cut short, is formatted
 * with snprintf() cast to void.
 *
 * It's defined in test_fmt.c rather than inline: GCC's
 * -Wformat-truncation=2 checks an inlined or specialised copy against the
 * full range of each argument's type, and reports a buffer sized for the
 * values the test passes.
 *
 * @param buf where to write the result
 * @param len the size of @p buf
 * @param fmt the printf() format
 */
ERNIC_PRINTF_FORMAT(3, 4)
void test_fmt(char *buf, size_t len, const char *fmt, ...);

#endif /* TEST_FMT_H */
