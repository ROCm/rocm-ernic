/*
 * Unit tests for the hash maps (ernic_map.h).
 *
 * Each value is a counter in slots[], which the map's destroy function
 * increments, so a test can tell which values the map dropped and how
 * often.  The growth test adds enough keys to make uthash double its bucket
 * array several times, then checks that every one is still found.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ernic_map.h"

#define N_SLOTS 5000U

/* Slot i's key in the growth test, which spreads N_SLOTS keys over the range */
#define KEY_STEP UINT32_C(858993)

static unsigned slots[N_SLOTS];
static unsigned null_destroyed; /* NULL values destroyed */
static int failures;

static void count_destroy(void *value)
{
    unsigned *slot = value;

    if (slot) {
        (*slot)++;
    } else {
        null_destroyed++;
    }
}

static void check(bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void reset_slots(void)
{
    memset(slots, 0, sizeof(slots));
    null_destroyed = 0;
}

static void test_empty(void)
{
    ErnicU32Map map;
    ErnicU32MapIter iter;
    uint32_t key = 7;
    void *value = &key;

    ernic_u32_map_init(&map, count_destroy);
    check(ernic_u32_map_size(&map) == 0, "empty: size");
    check(ernic_u32_map_lookup(&map, 0) == NULL, "empty: lookup");
    check(!ernic_u32_map_contains(&map, 0), "empty: contains");
    check(!ernic_u32_map_remove(&map, 0), "empty: remove");
    ernic_u32_map_iter_init(&iter, &map);
    check(!ernic_u32_map_iter_next(&iter, &key, &value), "empty: walk");
    check(key == 7 && value == &key, "empty: walk stored something");
    ernic_u32_map_destroy(&map);
    check(ernic_u32_map_size(&map) == 0, "empty: size after destroy");
}

static void test_insert_replace_remove(void)
{
    ErnicU32Map map;

    reset_slots();
    ernic_u32_map_init(&map, count_destroy);

    ernic_u32_map_insert(&map, 1, &slots[1]);
    ernic_u32_map_insert(&map, UINT32_MAX, &slots[2]);
    ernic_u32_map_insert(&map, 0, NULL);
    check(ernic_u32_map_size(&map) == 3, "insert: size");
    check(ernic_u32_map_lookup(&map, 1) == &slots[1], "insert: lookup 1");
    check(ernic_u32_map_lookup(&map, UINT32_MAX) == &slots[2],
          "insert: lookup UINT32_MAX");
    check(ernic_u32_map_lookup(&map, 0) == NULL, "insert: NULL value");
    check(ernic_u32_map_contains(&map, 0), "insert: contains NULL value");
    check(!ernic_u32_map_contains(&map, 2), "insert: contains absent key");

    /* Replacing drops the old value, not the new one */
    ernic_u32_map_insert(&map, 1, &slots[3]);
    check(ernic_u32_map_size(&map) == 3, "replace: size");
    check(ernic_u32_map_lookup(&map, 1) == &slots[3], "replace: lookup");
    check(slots[1] == 1 && slots[3] == 0, "replace: old value destroyed");

    check(ernic_u32_map_remove(&map, 1), "remove: found");
    check(slots[3] == 1, "remove: value destroyed");
    check(!ernic_u32_map_contains(&map, 1), "remove: gone");
    check(!ernic_u32_map_remove(&map, 1), "remove: twice");
    check(slots[3] == 1, "remove: twice destroyed again");
    check(ernic_u32_map_size(&map) == 2, "remove: size");

    /* A NULL value reaches the destroy function, as GLib's tables do */
    ernic_u32_map_destroy(&map);
    check(slots[2] == 1, "destroy: value destroyed");
    check(null_destroyed == 1, "destroy: NULL value destroyed");
    check(ernic_u32_map_size(&map) == 0, "destroy: size");

    /* The map is reusable after destroy */
    ernic_u32_map_insert(&map, 5, &slots[5]);
    check(ernic_u32_map_lookup(&map, 5) == &slots[5], "reuse: lookup");
    ernic_u32_map_destroy(&map);
    check(slots[5] == 1, "reuse: value destroyed");
}

static void test_no_destroy(void)
{
    ErnicU32Map map;

    reset_slots();
    ernic_u32_map_init(&map, NULL);
    ernic_u32_map_insert(&map, 1, &slots[1]);
    ernic_u32_map_insert(&map, 1, &slots[2]);
    ernic_u32_map_insert(&map, 2, &slots[3]);
    check(ernic_u32_map_remove(&map, 2), "no destroy: remove");
    ernic_u32_map_destroy(&map);
    check(slots[1] == 0 && slots[2] == 0 && slots[3] == 0,
          "no destroy: values left alone");
}

static void test_growth_and_walk(void)
{
    ErnicU32Map map;
    ErnicU32MapIter iter;
    uint32_t key;
    void *value;
    size_t walked = 0;
    bool walk_ok = true;
    bool lookup_ok = true;

    reset_slots();
    ernic_u32_map_init(&map, count_destroy);

    for (uint32_t i = 0; i < N_SLOTS; i++) {
        ernic_u32_map_insert(&map, i * KEY_STEP, &slots[i]);
    }
    check(ernic_u32_map_size(&map) == N_SLOTS, "growth: size");

    for (uint32_t i = 0; i < N_SLOTS; i++) {
        if (ernic_u32_map_lookup(&map, i * KEY_STEP) != &slots[i]) {
            lookup_ok = false;
        }
    }
    check(lookup_ok, "growth: every key found");

    /* The walk visits each entry once, with its own value */
    ernic_u32_map_iter_init(&iter, &map);
    while (ernic_u32_map_iter_next(&iter, &key, &value)) {
        size_t i = (size_t)(key / KEY_STEP);

        if (key % KEY_STEP != 0 || i >= N_SLOTS || value != &slots[i] ||
            slots[i] != 0) {
            walk_ok = false;
            break;
        }
        slots[i] = 100; /* seen, and not a destroy count */
        walked++;
    }
    check(walk_ok, "walk: entries");
    check(walked == N_SLOTS, "walk: count");
    reset_slots();

    /* Remove the even keys, which leaves the odd ones */
    for (uint32_t i = 0; i < N_SLOTS; i += 2) {
        if (!ernic_u32_map_remove(&map, i * KEY_STEP)) {
            lookup_ok = false;
        }
    }
    check(lookup_ok, "growth: remove every even key");
    check(ernic_u32_map_size(&map) == N_SLOTS / 2, "growth: size after");
    for (uint32_t i = 0; i < N_SLOTS; i++) {
        bool odd = i % 2 == 1;
        bool held = ernic_u32_map_contains(&map, i * KEY_STEP);

        if ((odd && !held) || (!odd && held) || slots[i] != (odd ? 0U : 1U)) {
            lookup_ok = false;
        }
    }
    check(lookup_ok, "growth: odd keys left, even values destroyed");

    ernic_u32_map_destroy(&map);
    for (uint32_t i = 0; i < N_SLOTS; i++) {
        if (slots[i] != 1) {
            lookup_ok = false;
        }
    }
    check(lookup_ok, "growth: destroy drops every value once");
}

int main(void)
{
    test_empty();
    test_insert_replace_remove();
    test_no_destroy();
    test_growth_and_walk();

    if (failures) {
        printf("\n%d failure(s)\n", failures);
    } else {
        printf("all ernic_map tests passed\n");
    }
    return failures;
}
