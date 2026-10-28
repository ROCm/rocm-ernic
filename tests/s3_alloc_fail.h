/*
 * s3_alloc_fail.h — make the S3 unit tests' allocations fail on demand
 *
 * The S3 test executables are linked with --wrap for malloc(), calloc(),
 * realloc(), strdup() and getentropy() (tests/CMakeLists.txt), so every
 * call to them from the test's own objects -- the S3 sources and the test
 * itself -- goes through s3_alloc_fail.c first.  Calls made inside libc
 * are not wrapped, and a call that is not failed goes to the real
 * function, so the sanitizers and valgrind still see every allocation.
 *
 * Not thread-safe: the S3 tests run on one thread.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef S3_ALLOC_FAIL_H
#define S3_ALLOC_FAIL_H

#include <stdbool.h>

/*
 * Fail one allocation: the @nth malloc(), calloc(), realloc() or strdup()
 * call from now, counting from 0, and none after it.
 */
void s3_alloc_fail_arm(unsigned nth);

/* Stop failing allocations.  True if one was failed since the arm. */
bool s3_alloc_fail_disarm(void);

/* While @fail, getentropy() fails with ENOSYS. */
void s3_alloc_fail_entropy(bool fail);

/*
 * While @fixed, getentropy() fills its buffer with 0, 1, 2, ... rather
 * than random bytes, so a test can know a key the code under test drew.
 */
void s3_alloc_fail_entropy_fixed(bool fixed);

#endif /* S3_ALLOC_FAIL_H */
