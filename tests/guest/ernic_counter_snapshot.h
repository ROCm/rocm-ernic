/*
 * Counter snapshot validation shared by the guest check and host unit tests.
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ERNIC_COUNTER_SNAPSHOT_H
#define ERNIC_COUNTER_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MAX_COUNTERS 64u
#define NAME_LEN     64u

struct counters {
    size_t n;
    char name[MAX_COUNTERS][NAME_LEN];
    uint64_t value[MAX_COUNTERS];
};

/* Sysfs emits unsigned decimal digits with an optional final newline. */
static inline bool parse_counter_value(const char *text, uint64_t *out)
{
    uint64_t value = 0;

    if (!text || !out || *text < '0' || *text > '9')
        return false;
    while (*text >= '0' && *text <= '9') {
        unsigned int digit = (unsigned int)(*text++ - '0');

        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    if (*text == '\n')
        text++;
    if (*text != '\0')
        return false;
    *out = value;
    return true;
}

/* A reordered directory listing is fine; missing/new/duplicate names are not.
 */
static inline bool counter_sets_match(const struct counters *a,
                                      const struct counters *b)
{
    if (!a->n || a->n > MAX_COUNTERS || a->n != b->n)
        return false;
    for (size_t i = 0; i < a->n; i++) {
        unsigned int found = 0;

        for (size_t j = 0; j < i; j++)
            if (strcmp(a->name[i], a->name[j]) == 0)
                return false;
        for (size_t j = 0; j < b->n; j++)
            found += strcmp(a->name[i], b->name[j]) == 0;
        if (found != 1)
            return false;
    }
    return true;
}

static inline bool counter_delta(uint64_t before, uint64_t after,
                                 uint64_t *delta)
{
    if (after < before)
        return false;
    *delta = after - before;
    return true;
}

#endif
