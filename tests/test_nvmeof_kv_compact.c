/*
 * test_nvmeof_kv_compact.c — unit tests for kv_compact()
 *
 * kv_compact() is called from nvmeof_kv_delete() when dead tombstones
 * outnumber live keys and the dead count crosses a minimum:
 *
 *   dead = occupied - live; dead > live && dead >= 64
 *
 * This file drives that path and also the calloc-failure path that leaves
 * the table intact as a best-effort degraded state.
 *
 * --wrap=calloc routes kv_compact()'s calloc through __wrap_calloc, which
 * can be armed to return NULL once so the allocation-failure path is
 * reachable without changing production code.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvmeof/nvmeof_kv.h"
#include "rocm-ernic-warnings.h"

static int failures;

static void check(const char *name, bool cond, const char *msg)
{
    if (!cond) {
        failures++;
        printf("FAIL %-30s: %s\n", name, msg);
    }
}

/* Armed: fail the next calloc call and disarm. */
static bool calloc_armed;

ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_OFF
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
/* NOLINTBEGIN(misc-use-internal-linkage) */
void *__real_calloc(size_t nmemb, size_t size);
void *__wrap_calloc(size_t nmemb, size_t size);

void *__wrap_calloc(size_t nmemb, size_t size)
{
    if (calloc_armed) {
        calloc_armed = false;
        return NULL;
    }
    return __real_calloc(nmemb, size);
}
/* NOLINTEND(misc-use-internal-linkage) */
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_ON

static void make_key(struct nvmeof_kv_key *k, uint32_t idx)
{
    memset(k->bytes, 0, sizeof(k->bytes));
    k->bytes[0] = (uint8_t)(idx & 0xffU);
    k->bytes[1] = (uint8_t)((idx >> 8) & 0xffU);
    k->len = 2;
}

static bool insert_range(struct nvmeof_kv *kv, uint32_t first, uint32_t n)
{
    static const uint8_t v = 0x42;

    for (uint32_t i = 0; i < n; i++) {
        struct nvmeof_kv_key k;
        make_key(&k, first + i);
        if (!nvmeof_kv_put(kv, &k, &v, sizeof(v))) {
            return false;
        }
    }
    return true;
}

static bool delete_range(struct nvmeof_kv *kv, uint32_t first, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        struct nvmeof_kv_key k;
        make_key(&k, first + i);
        if (!nvmeof_kv_delete(kv, &k)) {
            return false;
        }
    }
    return true;
}

/*
 * Insert 65 keys, delete 64 of them.  The 64th delete satisfies
 * dead=64 >= 64 and dead=64 > live=1, triggering kv_compact().
 * After compaction, occupied equals live and the surviving key
 * remains accessible.
 */
static void test_compact_success(void)
{
    struct nvmeof_kv *kv = nvmeof_kv_create(256, UINT64_C(1) << 30);
    if (kv == NULL) {
        failures++;
        printf("FAIL kv-compact: create failed\n");
        return;
    }

    check("kv-compact", insert_range(kv, 0, 65), "65 inserts failed");
    check("kv-compact", nvmeof_kv_count(kv) == 65,
          "count after 65 inserts wrong");

    check("kv-compact", delete_range(kv, 0, 64), "64 deletes failed");

    struct nvmeof_kv_key survivor;
    make_key(&survivor, 64);
    check("kv-compact", nvmeof_kv_exists(kv, &survivor),
          "surviving key lost after compaction");
    check("kv-compact", nvmeof_kv_count(kv) == 1,
          "count is not 1 after compaction");

    /* After compaction occupied equals live; inserting a new key works. */
    struct nvmeof_kv_key fresh;
    make_key(&fresh, 999);
    static const uint8_t v = 0x01;
    check("kv-compact", nvmeof_kv_put(kv, &fresh, &v, sizeof(v)),
          "insert after compaction failed");
    check("kv-compact", nvmeof_kv_count(kv) == 2,
          "count after post-compact insert wrong");

    nvmeof_kv_destroy(kv);
}

/*
 * Same trigger as test_compact_success, but kv_compact()'s calloc is
 * armed to fail.  The table must stay intact and the surviving key
 * must remain accessible.
 */
static void test_compact_alloc_fail(void)
{
    struct nvmeof_kv *kv = nvmeof_kv_create(256, UINT64_C(1) << 30);
    if (kv == NULL) {
        failures++;
        printf("FAIL kv-compact-fail: create failed\n");
        return;
    }

    check("kv-compact-fail", insert_range(kv, 0, 65), "65 inserts failed");

    /* Delete 63 without crossing the compact threshold:
     * dead=63 < 64, so no compaction yet. */
    check("kv-compact-fail", delete_range(kv, 0, 63),
          "first 63 deletes failed");

    /* 64th delete: dead becomes 64, live becomes 1 — both thresholds met.
     * Arm the wrapper so kv_compact()'s calloc returns NULL. */
    calloc_armed = true;
    struct nvmeof_kv_key last_del;
    make_key(&last_del, 63);
    check("kv-compact-fail", nvmeof_kv_delete(kv, &last_del),
          "the 64th delete failed");
    check("kv-compact-fail", !calloc_armed,
          "calloc was not called by kv_compact");

    /* Table stays intact despite the failed compact. */
    struct nvmeof_kv_key survivor;
    make_key(&survivor, 64);
    check("kv-compact-fail", nvmeof_kv_exists(kv, &survivor),
          "surviving key lost after failed compaction");
    check("kv-compact-fail", nvmeof_kv_count(kv) == 1,
          "count wrong after failed compaction");

    nvmeof_kv_destroy(kv);
}

int main(void)
{
    test_compact_success();
    test_compact_alloc_fail();

    if (failures == 0) {
        printf("PASS nvmeof-kv-compact\n");
    } else {
        printf("%d nvmeof-kv-compact check(s) failed\n", failures);
    }
    return failures != 0 ? 1 : 0;
}
