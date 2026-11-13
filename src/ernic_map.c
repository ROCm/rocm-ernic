/*
 * Hash maps keyed by a 32-bit number, over uthash
 *
 * This is the only file that includes uthash.h, so its macros, which use
 * the GNU extension __typeof, expand nowhere else (see AGENTS.md).
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "ernic_map.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ernic_alloc.h"
#include "ernic_log.h"
#include "ernic_siphash.h"

static uint32_t ernic_map_hash(const void *key, size_t len);

/*
 * uthash's settings, which it reads when its macros expand.  Its tables come
 * from ernic_xmalloc(), which aborts when out of memory, and anything else
 * it finds fatal aborts too (uthash's default is exit(-1)).  Its default
 * hash is unkeyed, and when one spreads keys badly it stops growing the
 * table, so colliding keys would leave long chains.
 */
#define uthash_malloc(sz) ernic_xmalloc(sz)
#define uthash_fatal(msg) ernic_log_fatal("ernic_map: %s", msg)
#define HASH_FUNCTION(keyptr, keylen, hashv) \
    ((hashv) = ernic_map_hash((keyptr), (keylen)))

#include <uthash.h>

struct ErnicU32MapEntry {
    uint32_t key;
    void *value;
    UT_hash_handle hh;
};

/* The hash key, drawn once per process by ernic_map_hash_key() */
static uint8_t hash_key[ERNIC_SIPHASH_KEY_LEN];
static pthread_once_t hash_key_once = PTHREAD_ONCE_INIT;

static void hash_key_draw(void)
{
    if (getentropy(hash_key, sizeof(hash_key)) != 0) {
        ernic_log_fatal("ernic_map: no random hash key: %s", strerror(errno));
    }
}

/*
 * The hash key, drawn on first use.  pthread_once() orders the draw before
 * every caller's read, whichever thread drew it.
 */
static const uint8_t *ernic_map_hash_key(void)
{
    int err = pthread_once(&hash_key_once, hash_key_draw);

    if (err != 0) {
        ernic_log_fatal("ernic_map: pthread_once failed: %s", strerror(err));
    }
    return hash_key;
}

/* uthash's HASH_FUNCTION: keyed SipHash, cut to the 32 bits uthash keeps */
static uint32_t ernic_map_hash(const void *key, size_t len)
{
    uint64_t h = ernic_siphash24(ernic_map_hash_key(), key, len);

    return (uint32_t)(h & UINT32_MAX);
}

/* @p map's entry for @p key, or NULL */
static struct ErnicU32MapEntry *u32_map_find(const ErnicU32Map *map,
                                             uint32_t key)
{
    struct ErnicU32MapEntry *e;

    HASH_FIND(hh, map->head, &key, sizeof(key), e);
    return e;
}

/* Take @p e, which @p map holds, out of it, and free it and its value */
static void u32_map_drop(ErnicU32Map *map, struct ErnicU32MapEntry *e)
{
    void *value = e->value;

    HASH_DEL(map->head, e);
    free(e);
    if (map->value_destroy) {
        map->value_destroy(value);
    }
}

void ernic_u32_map_init(ErnicU32Map *map, ErnicMapDestroyFn value_destroy)
{
    (void)ernic_map_hash_key();
    map->head = NULL;
    map->value_destroy = value_destroy;
}

void ernic_u32_map_destroy(ErnicU32Map *map)
{
    struct ErnicU32MapEntry *e = map->head;

    /*
     * Free uthash's table first, then walk the entries, which stay linked
     * in insertion order.  (Deleting them one at a time from the head, the
     * clang analyzer can't tell that the head has no predecessor, and
     * reports a use after free.)
     */
    HASH_CLEAR(hh, map->head);
    while (e) {
        struct ErnicU32MapEntry *next = e->hh.next;
        void *value = e->value;

        free(e);
        if (map->value_destroy) {
            map->value_destroy(value);
        }
        e = next;
    }
}

void ernic_u32_map_insert(ErnicU32Map *map, uint32_t key, void *value)
{
    struct ErnicU32MapEntry *e = u32_map_find(map, key);

    if (e) {
        void *old = e->value;

        e->value = value;
        if (map->value_destroy) {
            map->value_destroy(old);
        }
        return;
    }

    e = ernic_xcalloc(1, sizeof(*e));
    e->key = key;
    e->value = value;
    HASH_ADD(hh, map->head, key, sizeof(e->key), e);
}

void *ernic_u32_map_lookup(const ErnicU32Map *map, uint32_t key)
{
    struct ErnicU32MapEntry *e = u32_map_find(map, key);

    return e ? e->value : NULL;
}

bool ernic_u32_map_contains(const ErnicU32Map *map, uint32_t key)
{
    return u32_map_find(map, key) != NULL;
}

bool ernic_u32_map_remove(ErnicU32Map *map, uint32_t key)
{
    struct ErnicU32MapEntry *e = u32_map_find(map, key);

    if (!e) {
        return false;
    }
    u32_map_drop(map, e);
    return true;
}

size_t ernic_u32_map_size(const ErnicU32Map *map)
{
    return HASH_COUNT(map->head);
}

void ernic_u32_map_iter_init(ErnicU32MapIter *iter, const ErnicU32Map *map)
{
    iter->next = map->head;
}

bool ernic_u32_map_iter_next(ErnicU32MapIter *iter, uint32_t *key, void **value)
{
    struct ErnicU32MapEntry *e = iter->next;

    if (!e) {
        return false;
    }
    iter->next = e->hh.next;
    *key = e->key;
    *value = e->value;
    return true;
}
