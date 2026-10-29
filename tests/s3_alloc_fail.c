/*
 * s3_alloc_fail.c — make the S3 unit tests' allocations fail on demand
 *
 * See s3_alloc_fail.h.  The linker's --wrap=SYM sends calls to SYM to
 * __wrap_SYM and makes __real_SYM name the real one, so those names are
 * fixed by the linker, not chosen here.
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

#include "rocm-ernic-warnings.h"
#include "s3_alloc_fail.h"

static bool armed;
static unsigned countdown; /* allocations to let through before failing */
static bool fired;
static bool entropy_fails;
static bool entropy_fixed;

void s3_alloc_fail_arm(unsigned nth)
{
    armed = true;
    countdown = nth;
    fired = false;
}

bool s3_alloc_fail_disarm(void)
{
    armed = false;
    return fired;
}

void s3_alloc_fail_entropy(bool fail)
{
    entropy_fails = fail;
}

void s3_alloc_fail_entropy_fixed(bool fixed)
{
    entropy_fixed = fixed;
}

static bool should_fail(void)
{
    if (!armed) {
        return false;
    }
    if (countdown > 0) {
        countdown--;
        return false;
    }
    armed = false;
    fired = true;
    errno = ENOMEM;
    return true;
}

/*
 * The names are --wrap's, so they are reserved identifiers by necessity, and
 * they can't be static, since the linker has to find them.
 */
ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_OFF
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
/* NOLINTBEGIN(misc-use-internal-linkage) */
void *__real_malloc(size_t size);
void *__real_calloc(size_t nmemb, size_t size);
void *__real_realloc(void *ptr, size_t size);
char *__real_strdup(const char *s);
int __real_getentropy(void *buf, size_t len);

void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t nmemb, size_t size);
void *__wrap_realloc(void *ptr, size_t size);
char *__wrap_strdup(const char *s);
int __wrap_getentropy(void *buf, size_t len);

void *__wrap_malloc(size_t size)
{
    return should_fail() ? NULL : __real_malloc(size);
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
    return should_fail() ? NULL : __real_calloc(nmemb, size);
}

/* A failed realloc() leaves the block as it was, as the real one does. */
void *__wrap_realloc(void *ptr, size_t size)
{
    return should_fail() ? NULL : __real_realloc(ptr, size);
}

char *__wrap_strdup(const char *s)
{
    return should_fail() ? NULL : __real_strdup(s);
}

int __wrap_getentropy(void *buf, size_t len)
{
    if (entropy_fails) {
        errno = ENOSYS;
        return -1;
    }
    if (entropy_fixed) {
        unsigned char *p = buf;
        for (size_t i = 0; i < len; i++) {
            p[i] = (unsigned char)i;
        }
        return 0;
    }
    return __real_getentropy(buf, len);
}
/* NOLINTEND(misc-use-internal-linkage) */
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_ON
