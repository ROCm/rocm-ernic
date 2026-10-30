/*
 * Monotonic time and sleeping.
 *
 * Intervals, timeouts and deadlines are measured on CLOCK_MONOTONIC, which
 * an NTP step or a change to the date doesn't move.  The wall clock is only
 * for timestamps something outside the process reads, such as an S3
 * object's Last-Modified.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef ERNIC_TIME_H
#define ERNIC_TIME_H

#include <stdint.h>
#include <time.h>

/*
 * Milliseconds on CLOCK_MONOTONIC.  The origin is unspecified (on Linux it's
 * boot), so the values mean something only when subtracted from each other.
 */
static inline uint64_t ernic_monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000U) + ((uint64_t)ts.tv_nsec / 1000000U);
}

/* Microseconds on CLOCK_MONOTONIC, from the same origin. */
static inline uint64_t ernic_monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000U) + ((uint64_t)ts.tv_nsec / 1000U);
}

/*
 * Sleep for us microseconds on CLOCK_MONOTONIC.  A signal handler ends the
 * sleep early, so a caller that loops on a flag the handler sets sees it at
 * once.
 */
static inline void ernic_sleep_us(uint64_t us)
{
    struct timespec ts = {
        .tv_sec = (time_t)(us / 1000000U),
        .tv_nsec = (long)(us % 1000000U) * 1000,
    };

    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}

#endif /* ERNIC_TIME_H */
