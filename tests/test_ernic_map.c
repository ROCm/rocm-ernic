/*
 * Unit tests for the hash maps (ernic_map.h).
 *
 * Each value is a counter in slots[], which the map's destroy function
 * increments, so a test can tell which values the map dropped and how
 * often.  The growth test adds enough keys to make uthash double its bucket
 * array several times, then checks that every one is still found.  The GID
 * map's keys differ from each other in one byte, at every position, so a
 * key compared or hashed short of its full length collides.
 *
 * The program is linked with --wrap=malloc,--wrap=calloc
 * (tests/CMakeLists.txt), so a case can fail the next allocation of either
 * kind: uthash's tables come from malloc() and the entries from calloc().
 * Running out of memory must abort, as must removing through a walk that
 * isn't at an entry.  Each of those runs in a child process, which must die
 * by SIGABRT with ernic_log_fatal()'s message.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ernic_gnu.h"
#include "ernic_map.h"
#include "rocm-ernic-warnings.h"

#define N_SLOTS 5000U

/* Slot i's key in the growth test, which spreads N_SLOTS keys over the range */
#define KEY_STEP UINT32_C(858993)

static unsigned slots[N_SLOTS];
static unsigned null_destroyed; /* NULL values destroyed */
static int failures;

/* While set, the next malloc() or calloc() fails and clears it */
static bool fail_next_malloc;
static bool fail_next_calloc;

/*
 * The names are --wrap's, so they are reserved identifiers by necessity, and
 * they can't be static, since the linker has to find them.
 */
ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_OFF
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
/* NOLINTBEGIN(misc-use-internal-linkage) */
void *__real_malloc(size_t size);
void *__real_calloc(size_t nmemb, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t nmemb, size_t size);

void *__wrap_malloc(size_t size)
{
    if (fail_next_malloc) {
        fail_next_malloc = false;
        errno = ENOMEM;
        return NULL;
    }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
    if (fail_next_calloc) {
        fail_next_calloc = false;
        errno = ENOMEM;
        return NULL;
    }
    return __real_calloc(nmemb, size);
}
/* NOLINTEND(misc-use-internal-linkage) */
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_ON

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

static void test_add_and_steal(void)
{
    ErnicU32Map map;
    void *value = &failures;

    reset_slots();
    ernic_u32_map_init(&map, count_destroy);

    /* A set: each key added once, with a NULL value */
    ernic_u32_map_add(&map, 10);
    ernic_u32_map_add(&map, 11);
    ernic_u32_map_add(&map, 10);
    check(ernic_u32_map_size(&map) == 2, "add: size");
    check(ernic_u32_map_contains(&map, 10) && ernic_u32_map_contains(&map, 11),
          "add: contains");
    check(ernic_u32_map_lookup(&map, 10) == NULL, "add: NULL value");
    check(null_destroyed == 1, "add: again replaces the NULL value");

    /* Stealing hands the value over without destroying it */
    ernic_u32_map_insert(&map, 1, &slots[1]);
    check(ernic_u32_map_steal(&map, 1, &value), "steal: found");
    check(value == &slots[1], "steal: value");
    check(slots[1] == 0, "steal: value not destroyed");
    check(!ernic_u32_map_contains(&map, 1), "steal: gone");
    value = &failures;
    check(!ernic_u32_map_steal(&map, 1, &value), "steal: twice");
    check(value == &failures, "steal: twice stored something");

    /* The value may be left behind */
    ernic_u32_map_insert(&map, 2, &slots[2]);
    check(ernic_u32_map_steal(&map, 2, NULL), "steal: no value wanted");
    check(slots[2] == 0 && !ernic_u32_map_contains(&map, 2),
          "steal: no value wanted, gone and not destroyed");
    check(ernic_u32_map_size(&map) == 2, "steal: size");

    ernic_u32_map_destroy(&map);
    check(null_destroyed == 3, "add: destroy drops the NULL values");
}

static void test_walk_remove_and_steal(void)
{
    ErnicU32Map map;
    ErnicU32MapIter iter;
    uint32_t key;
    void *value;
    unsigned seen[100] = {0};
    bool ok = true;

    reset_slots();
    ernic_u32_map_init(&map, count_destroy);
    for (uint32_t i = 0; i < 100; i++) {
        ernic_u32_map_insert(&map, i, &slots[i]);
    }

    /* Remove the multiples of 3, steal the other even keys, keep the rest */
    ernic_u32_map_iter_init(&iter, &map);
    while (ernic_u32_map_iter_next(&iter, &key, &value)) {
        if (key >= 100 || value != &slots[key]) {
            ok = false;
            break;
        }
        seen[key]++;
        if (key % 3 == 0) {
            ernic_u32_map_iter_remove(&iter);
        } else if (key % 2 == 0) {
            ernic_u32_map_iter_steal(&iter);
        }
    }
    check(ok, "walk remove: entries");
    for (uint32_t i = 0; i < 100; i++) {
        bool removed = i % 3 == 0;
        bool stolen = !removed && i % 2 == 0;
        bool kept = !removed && !stolen;

        if (seen[i] != 1 || slots[i] != (removed ? 1U : 0U) ||
            ernic_u32_map_contains(&map, i) != kept) {
            ok = false;
        }
    }
    check(ok, "walk remove: each seen once, removed destroyed, stolen not");
    check(ernic_u32_map_size(&map) == 33, "walk remove: size");

    /* A walk without the key or the value, removing everything */
    ernic_u32_map_iter_init(&iter, &map);
    while (ernic_u32_map_iter_next(&iter, NULL, NULL)) {
        ernic_u32_map_iter_remove(&iter);
    }
    check(ernic_u32_map_size(&map) == 0, "walk remove: all");
    for (uint32_t i = 0; i < 100; i++) {
        bool kept = i % 3 != 0 && i % 2 != 0;

        if (kept && slots[i] != 1) {
            ok = false;
        }
    }
    check(ok, "walk remove: all destroyed");

    /* The emptied map still works */
    ernic_u32_map_insert(&map, 7, &slots[7]);
    check(ernic_u32_map_lookup(&map, 7) == &slots[7], "walk remove: reuse");
    ernic_u32_map_destroy(&map);
}

static void test_new_and_free(void)
{
    ErnicU32Map *map;

    reset_slots();
    map = ernic_u32_map_new(count_destroy);
    check(ernic_u32_map_size(map) == 0, "new: empty");
    ernic_u32_map_insert(map, 3, &slots[3]);
    ernic_u32_map_insert(map, 4, &slots[4]);
    ernic_u32_map_free(map);
    check(slots[3] == 1 && slots[4] == 1, "free: values destroyed");
    ernic_u32_map_free(NULL);
}

/* The GID map's base key, and that key with byte @p pos changed by @p delta */
static void gid_key(uint8_t key[ERNIC_GID_MAP_KEY_LEN], int pos, uint8_t delta)
{
    for (unsigned i = 0; i < ERNIC_GID_MAP_KEY_LEN; i++) {
        key[i] = (uint8_t)(0x10U + i);
    }
    if (pos >= 0) {
        key[pos] = (uint8_t)(key[pos] + delta);
    }
}

static void test_gid_map(void)
{
    ErnicGidMap map;
    ErnicGidMap *heap;
    uint8_t key[ERNIC_GID_MAP_KEY_LEN];
    uint32_t value = 77;
    bool ok = true;

    ernic_gid_map_init(&map);
    gid_key(key, -1, 0);
    check(ernic_gid_map_size(&map) == 0, "gid empty: size");
    check(!ernic_gid_map_lookup(&map, key, &value), "gid empty: lookup");
    check(value == 77, "gid empty: lookup stored something");

    /* The base key, then one differing from it in each byte */
    ernic_gid_map_insert(&map, key, UINT32_MAX);
    for (int pos = 0; pos < (int)ERNIC_GID_MAP_KEY_LEN; pos++) {
        gid_key(key, pos, 1);
        ernic_gid_map_insert(&map, key, (uint32_t)pos);
    }
    check(ernic_gid_map_size(&map) == ERNIC_GID_MAP_KEY_LEN + 1, "gid: size");

    gid_key(key, -1, 0);
    check(ernic_gid_map_lookup(&map, key, &value) && value == UINT32_MAX,
          "gid: base key");
    for (int pos = 0; pos < (int)ERNIC_GID_MAP_KEY_LEN; pos++) {
        gid_key(key, pos, 1);
        if (!ernic_gid_map_lookup(&map, key, &value) ||
            value != (uint32_t)pos) {
            ok = false;
        }
        gid_key(key, pos, 2);
        if (ernic_gid_map_lookup(&map, key, &value)) {
            ok = false;
        }
    }
    check(ok, "gid: each one-byte difference is its own key");

    /* Inserting a key again replaces its value */
    gid_key(key, 15, 1);
    ernic_gid_map_insert(&map, key, 1234);
    check(ernic_gid_map_size(&map) == ERNIC_GID_MAP_KEY_LEN + 1,
          "gid replace: size");
    check(ernic_gid_map_lookup(&map, key, &value) && value == 1234,
          "gid replace: lookup");

    ernic_gid_map_destroy(&map);
    check(ernic_gid_map_size(&map) == 0, "gid destroy: size");
    check(!ernic_gid_map_lookup(&map, key, &value), "gid destroy: lookup");

    /* Reusable after destroy */
    ernic_gid_map_insert(&map, key, 5);
    check(ernic_gid_map_lookup(&map, key, &value) && value == 5,
          "gid reuse: lookup");
    ernic_gid_map_destroy(&map);

    heap = ernic_gid_map_new();
    ernic_gid_map_insert(heap, key, 6);
    check(ernic_gid_map_lookup(heap, key, &value) && value == 6,
          "gid new: lookup");
    ernic_gid_map_free(heap);
    ernic_gid_map_free(NULL);
}

/* ---- What must abort -------------------------------------------------- */

/*
 * Runs @p fn in a child process, which must die by SIGABRT having written
 * @p want to stderr.  The message tells an abort through ernic_log_fatal()
 * from one in the C library, such as a double free's.
 */
static void expect_abort(void (*fn)(void), const char *what, const char *want)
{
    char out[4096];
    size_t len = 0;
    ssize_t n;
    int fds[2];
    int status;
    pid_t pid;

    if (ernic_gnu_pipe2(fds, 0) != 0) {
        printf("FAIL %s: pipe failed\n", what);
        failures++;
        return;
    }
    (void)fflush(stdout);
    pid = fork();
    if (pid < 0) {
        printf("FAIL %s: fork failed\n", what);
        failures++;
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0) {
        /* The fatal message goes to the parent, not the test log */
        close(fds[0]);
        if (dup2(fds[1], STDERR_FILENO) < 0) {
            _exit(2);
        }
        alarm(30);
        fn();
        _exit(0);
    }

    close(fds[1]);
    while (len < sizeof(out) - 1 &&
           (n = read(fds[0], out + len, sizeof(out) - 1 - len)) > 0) {
        len += (size_t)n;
    }
    out[len] = '\0';
    close(fds[0]);

    if (waitpid(pid, &status, 0) != pid) {
        printf("FAIL %s: waitpid failed\n", what);
        failures++;
    } else if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        printf("FAIL %s did not abort\n", what);
        failures++;
    } else if (!strstr(out, want)) {
        printf("FAIL %s aborted without \"%s\": %s\n", what, want, out);
        failures++;
    }
}

/* Each frees what it made, so a function that returns doesn't leak */

/* The first entry's uthash table */
static void u32_insert_no_table_memory(void)
{
    ErnicU32Map map;

    ernic_u32_map_init(&map, NULL);
    fail_next_malloc = true;
    ernic_u32_map_insert(&map, 1, NULL);
    ernic_u32_map_destroy(&map);
}

static void u32_insert_no_entry_memory(void)
{
    ErnicU32Map map;

    ernic_u32_map_init(&map, NULL);
    ernic_u32_map_insert(&map, 1, NULL);
    fail_next_calloc = true;
    ernic_u32_map_insert(&map, 2, NULL);
    ernic_u32_map_destroy(&map);
}

/* Entries come from calloc(), so the next malloc() is a bigger bucket array */
static void u32_grow_no_memory(void)
{
    ErnicU32Map map;

    ernic_u32_map_init(&map, NULL);
    ernic_u32_map_insert(&map, 0, NULL);
    fail_next_malloc = true;
    for (uint32_t i = 1; i < N_SLOTS; i++) {
        ernic_u32_map_insert(&map, i * KEY_STEP, NULL);
    }
    ernic_u32_map_destroy(&map);
}

static void u32_new_no_memory(void)
{
    fail_next_malloc = true;
    ernic_u32_map_free(ernic_u32_map_new(NULL));
}

static void gid_insert_no_entry_memory(void)
{
    ErnicGidMap map;
    uint8_t key[ERNIC_GID_MAP_KEY_LEN];

    gid_key(key, -1, 0);
    ernic_gid_map_init(&map);
    fail_next_calloc = true;
    ernic_gid_map_insert(&map, key, 1);
    ernic_gid_map_destroy(&map);
}

static void gid_new_no_memory(void)
{
    fail_next_malloc = true;
    ernic_gid_map_free(ernic_gid_map_new());
}

static void iter_remove_before_next(void)
{
    ErnicU32Map map;
    ErnicU32MapIter iter;

    ernic_u32_map_init(&map, NULL);
    ernic_u32_map_insert(&map, 1, NULL);
    ernic_u32_map_iter_init(&iter, &map);
    ernic_u32_map_iter_remove(&iter);
    ernic_u32_map_destroy(&map);
}

static void iter_remove_twice(void)
{
    ErnicU32Map map;
    ErnicU32MapIter iter;

    ernic_u32_map_init(&map, NULL);
    ernic_u32_map_insert(&map, 1, NULL);
    ernic_u32_map_insert(&map, 2, NULL);
    ernic_u32_map_iter_init(&iter, &map);
    if (ernic_u32_map_iter_next(&iter, NULL, NULL)) {
        ernic_u32_map_iter_remove(&iter);
        ernic_u32_map_iter_remove(&iter);
    }
    ernic_u32_map_destroy(&map);
}

static void iter_steal_past_end(void)
{
    ErnicU32Map map;
    ErnicU32MapIter iter;

    ernic_u32_map_init(&map, NULL);
    ernic_u32_map_insert(&map, 1, NULL);
    ernic_u32_map_iter_init(&iter, &map);
    if (ernic_u32_map_iter_next(&iter, NULL, NULL) &&
        !ernic_u32_map_iter_next(&iter, NULL, NULL)) {
        ernic_u32_map_iter_steal(&iter);
    }
    ernic_u32_map_destroy(&map);
}

static void test_aborts(void)
{
    static const char oom[] = "FATAL: out of memory";
    static const char no_entry[] = "with no entry to remove";

    expect_abort(u32_insert_no_table_memory, "u32 insert, no table memory",
                 oom);
    expect_abort(u32_insert_no_entry_memory, "u32 insert, no entry memory",
                 oom);
    expect_abort(u32_grow_no_memory, "u32 growth, no memory", oom);
    expect_abort(u32_new_no_memory, "u32 new, no memory", oom);
    expect_abort(gid_insert_no_entry_memory, "gid insert, no entry memory",
                 oom);
    expect_abort(gid_new_no_memory, "gid new, no memory", oom);
    expect_abort(iter_remove_before_next, "walk remove before next", no_entry);
    expect_abort(iter_remove_twice, "walk remove twice", no_entry);
    expect_abort(iter_steal_past_end, "walk steal past the end", no_entry);
}

int main(void)
{
    test_empty();
    test_insert_replace_remove();
    test_no_destroy();
    test_growth_and_walk();
    test_add_and_steal();
    test_walk_remove_and_steal();
    test_new_and_free();
    test_gid_map();
    test_aborts();

    if (failures) {
        printf("\n%d failure(s)\n", failures);
    } else {
        printf("all ernic_map tests passed\n");
    }
    return failures;
}
