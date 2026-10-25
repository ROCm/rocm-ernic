/*
 * ernic_err.h - fill in a caller's error-message buffer
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef ERNIC_ERR_H
#define ERNIC_ERR_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

/**
 * ernic_set_err() - format an error message into @err
 *
 * For the parsers and setup functions that report failure through an
 * optional (@err, @err_len) buffer. Does nothing if @err is NULL or
 * @err_len is 0. A message too long for the buffer is truncated: it is
 * for a human to read.
 */
__attribute__((format(printf, 3, 4))) static inline void ernic_set_err(
    char *err, size_t err_len, const char *fmt, ...)
{
    va_list ap;

    if (!err || err_len == 0) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

#endif /* ERNIC_ERR_H */
