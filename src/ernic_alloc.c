/*
 * Allocation that aborts when out of memory.  See ernic_alloc.h.
 *
 * Every allocation here goes through malloc(), calloc(), or realloc(), so a
 * test that fails them with --wrap reaches these too.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "ernic_alloc.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ernic_log.h"

void *ernic_xmalloc(size_t size)
{
    /* malloc(0) may return NULL, which would look like a failure */
    void *p = malloc(size ? size : 1);

    if (!p) {
        ernic_log_fatal("out of memory allocating %zu bytes", size);
    }
    return p;
}

void *ernic_xcalloc(size_t nmemb, size_t size)
{
    void *p;

    if (size != 0 && nmemb > SIZE_MAX / size) {
        ernic_log_fatal("an array of %zu elements of %zu bytes is too large",
                        nmemb, size);
    }
    /* calloc() of nothing may return NULL, which would look like a failure */
    if (nmemb == 0 || size == 0) {
        nmemb = 1;
        size = 1;
    }
    p = calloc(nmemb, size);
    if (!p) {
        ernic_log_fatal("out of memory allocating %zu elements of %zu bytes",
                        nmemb, size);
    }
    return p;
}

void *ernic_xreallocarray(void *ptr, size_t nmemb, size_t size)
{
    size_t bytes;
    void *p;

    if (size != 0 && nmemb > SIZE_MAX / size) {
        ernic_log_fatal("an array of %zu elements of %zu bytes is too large",
                        nmemb, size);
    }
    bytes = nmemb * size;
    /* realloc() to nothing may free the block and return NULL */
    p = realloc(ptr, bytes ? bytes : 1);
    if (!p) {
        ernic_log_fatal("out of memory resizing an array to %zu elements of "
                        "%zu bytes",
                        nmemb, size);
    }
    return p;
}

/* A copy of the @p len bytes at @p s, terminated */
static char *xmemdup_str(const char *s, size_t len)
{
    char *copy;

    /* len + 1 can't wrap: s points to len bytes in memory */
    copy = ernic_xmalloc(len + 1);
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

char *ernic_xstrdup(const char *s)
{
    return xmemdup_str(s, strlen(s));
}

char *ernic_xstrndup(const char *s, size_t n)
{
    return xmemdup_str(s, strnlen(s, n));
}

char *ernic_xasprintf(const char *fmt, ...)
{
    va_list ap;
    char *buf;
    size_t len;
    int n;

    /* Measure, then format into a block of that size */
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        ernic_log_fatal("can't format \"%s\": %s", fmt, strerror(errno));
    }
    len = (size_t)n;
    buf = ernic_xmalloc(len + 1);

    va_start(ap, fmt);
    n = vsnprintf(buf, len + 1, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n != len) {
        ernic_log_fatal("\"%s\" formatted to a different length", fmt);
    }
    return buf;
}
