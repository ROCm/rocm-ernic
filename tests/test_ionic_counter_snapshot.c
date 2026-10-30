/*
 * Host tests of the guest counter oracle, without RDMA hardware.
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "guest/ernic_counter_snapshot.h"

static int failures;

static void report(bool ok, const char *description, const char *expected)
{
    printf("%s %s; expected: %s\n", ok ? "ok" : "FAIL", description, expected);
    if (!ok) {
        failures++;
    }
}

/* Three tables, one per function in ernic_counter_snapshot.h. Every row
 * prints an ok or FAIL line, and the exit status is 1 if any failed. */
int main(void)
{
    /* parse_counter_value(): the text of a sysfs counter file. Only plain
     * unsigned decimal, with at most one final newline, is a value;
     * anything else is rejected without touching the output. */
    static const struct {
        const char *description;
        const char *text;
        const char *expected;
        bool accepted;
        uint64_t value;
    } parse[] = {
        {"positive: decimal newline", "123\n", "123", true, 123},
        {"boundary: zero", "0", "0", true, 0},
        {"boundary: UINT64_MAX", "18446744073709551615\n", "UINT64_MAX", true,
         UINT64_MAX},
        {"boundary: overflow by one", "18446744073709551616",
         "reject; output unchanged", false, 0},
        {"negative: minus sign", "-1", "reject; output unchanged", false, 0},
        {"negative: plus sign", "+1", "reject; output unchanged", false, 0},
        {"negative: leading space", " 1", "reject; output unchanged", false, 0},
        {"negative: trailing junk", "1x", "reject; output unchanged", false, 0},
        {"negative: empty", "", "reject; output unchanged", false, 0},
        {"corner: second line", "1\n2", "reject; output unchanged", false, 0},
        {"corner: double newline", "1\n\n", "reject; output unchanged", false,
         0},
        {"corner: leading zeros", "00012\n", "12", true, 12},
    };
    /* counter_sets_match(): the counter names listed before and after a
     * run. Their order may change, but a missing, new, renamed or
     * duplicated name, or no counters at all, fails the match. */
    static const struct {
        const char *description;
        const char *expected;
        struct counters before;
        struct counters after;
        bool same;
    } sets[] = {
        {"positive: unchanged names",
         "accept",
         {2, {"a", "b"}, {0}},
         {2, {"a", "b"}, {0}},
         true},
        {"corner: reordered names",
         "accept",
         {2, {"a", "b"}, {0}},
         {2, {"b", "a"}, {0}},
         true},
        {"negative: missing unlisted name",
         "reject",
         {2, {"a", "b"}, {0}},
         {1, {"a"}, {0}},
         false},
        {"negative: newly appearing name",
         "reject",
         {1, {"a"}, {0}},
         {2, {"a", "b"}, {0}},
         false},
        {"negative: renamed counter",
         "reject",
         {2, {"a", "b"}, {0}},
         {2, {"a", "c"}, {0}},
         false},
        {"negative: duplicate before",
         "reject",
         {2, {"a", "a"}, {0}},
         {2, {"a", "b"}, {0}},
         false},
        {"negative: duplicate after",
         "reject",
         {2, {"a", "b"}, {0}},
         {2, {"a", "a"}, {0}},
         false},
        {"boundary: empty snapshots",
         "reject",
         {0, {{0}}, {0}},
         {0, {{0}}, {0}},
         false},
    };
    /* counter_delta(): a counter that went down was reset or wrapped, and
     * is rejected rather than read as a huge increase. */
    static const struct {
        const char *description;
        const char *expected;
        uint64_t before, after, delta;
        bool accepted;
    } deltas[] = {
        {"positive: increment", "delta 1", 9, 10, 1, true},
        {"boundary: unchanged", "delta 0", 10, 10, 0, true},
        {"boundary: maximum delta", "UINT64_MAX", 0, UINT64_MAX, UINT64_MAX,
         true},
        {"negative: reset", "reject decrease", 10, 0, 0, false},
        {"corner: apparent wrap", "reject decrease", UINT64_MAX, 0, 0, false},
    };

    /* Each output starts at 42, which no accepted row produces, so a
     * rejected input that wrote the output anyway fails its row. */
    for (size_t i = 0; i < sizeof(parse) / sizeof(parse[0]); i++) {
        uint64_t value = 42;
        bool ok = parse_counter_value(parse[i].text, &value);

        report(ok == parse[i].accepted && value == (ok ? parse[i].value : 42),
               parse[i].description, parse[i].expected);
    }
    for (size_t i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        report(counter_sets_match(&sets[i].before, &sets[i].after) ==
                   sets[i].same,
               sets[i].description, sets[i].expected);
    }
    for (size_t i = 0; i < sizeof(deltas) / sizeof(deltas[0]); i++) {
        uint64_t delta = 42;
        bool ok = counter_delta(deltas[i].before, deltas[i].after, &delta);

        report(ok == deltas[i].accepted && delta == (ok ? deltas[i].delta : 42),
               deltas[i].description, deltas[i].expected);
    }
    return failures ? 1 : 0;
}
