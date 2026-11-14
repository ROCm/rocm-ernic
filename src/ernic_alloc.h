/*
 * Allocation that aborts when out of memory
 *
 * The ernic_x*() functions never return NULL: when the process is out of
 * memory, they abort through ernic_log_fatal().  They are for fixed-size
 * internal allocations, where there's nothing useful to do but stop, and
 * which would otherwise each need an error path no one can test.  A size a
 * guest, peer, or client chooses must use malloc() and check the result
 * instead, so that a large request fails alone rather than taking down the
 * server.
 *
 * Free everything they return with free().  A zero size is allocated as a
 * one-byte block, so a zero-length result is a pointer like any other.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef ERNIC_ALLOC_H
#define ERNIC_ALLOC_H

#include <stddef.h>

#include "ernic_attributes.h"

/**
 * Allocate memory, uninitialized
 *
 * @param size how many bytes
 *
 * Returns the block, never NULL.
 */
void *ernic_xmalloc(size_t size);

/**
 * Allocate a zeroed array
 *
 * @param nmemb how many elements
 * @param size  the size of each
 *
 * Returns the block, never NULL.  Aborts through ernic_log_fatal() if
 * @p nmemb times @p size doesn't fit in a size_t.
 */
void *ernic_xcalloc(size_t nmemb, size_t size);

/**
 * Resize an array, keeping its contents
 *
 * @param ptr   the array, or NULL for a new one
 * @param nmemb how many elements it is to hold
 * @param size  the size of each
 *
 * Returns the array, which may have moved, never NULL; elements past the
 * old size are uninitialized.  Aborts through ernic_log_fatal() if @p nmemb
 * times @p size doesn't fit in a size_t.
 */
void *ernic_xreallocarray(void *ptr, size_t nmemb, size_t size);

/**
 * Copy a string
 *
 * @param s the string
 *
 * Returns the copy, never NULL.
 */
char *ernic_xstrdup(const char *s);

/**
 * Copy at most @p n bytes of a string
 *
 * @param s the string, which needn't be terminated if it is @p n bytes or
 *          longer
 * @param n the most bytes to copy
 *
 * Returns the copy, always terminated, never NULL.
 */
char *ernic_xstrndup(const char *s, size_t n);

/**
 * Format a string into a block just large enough for it
 *
 * @param fmt a printf() format, and its arguments after it
 *
 * Returns the string, never NULL.  Aborts through ernic_log_fatal() if the
 * arguments can't be formatted.
 */
char *ernic_xasprintf(const char *fmt, ...) ERNIC_PRINTF_FORMAT(1, 2);

#endif /* ERNIC_ALLOC_H */
