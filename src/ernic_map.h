/*
 * Hash maps keyed by a 32-bit number
 *
 * ErnicU32Map maps a uint32_t key, such as a QPN or a node id, to a
 * pointer.  It is a typed wrapper over uthash (third-party/uthash/), whose
 * macros expand only in ernic_map.c, so the code using a map never sees
 * them.  Keys are hashed with SipHash under a random per-process key, since
 * many come from guests and peers: with an unkeyed hash, keys chosen to
 * collide would make every lookup walk one long chain.
 *
 * A map holds no lock.  Protect it with the lock of the structure it sits
 * in, as any other field.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef ERNIC_MAP_H
#define ERNIC_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Frees a value the map drops; see ernic_u32_map_init() */
typedef void (*ErnicMapDestroyFn)(void *value);

/*
 * A map from uint32_t to void *.  Set it up with ernic_u32_map_init() and
 * touch the fields only through the functions here.
 */
typedef struct ErnicU32Map {
    /* uthash's table, NULL when empty; the entry type is private to
     * ernic_map.c */
    struct ErnicU32MapEntry *head;
    ErnicMapDestroyFn value_destroy;
} ErnicU32Map;

/* A walk over a map's entries; see ernic_u32_map_iter_init() */
typedef struct ErnicU32MapIter {
    struct ErnicU32MapEntry *next;
} ErnicU32MapIter;

/**
 * Set up an empty map
 *
 * @param map           the map
 * @param value_destroy called on a value when the map drops it (removed,
 *                      replaced, or emptied by ernic_u32_map_destroy()), or
 *                      NULL to leave values alone
 *
 * The first map a process sets up draws the hash key from getentropy(), and
 * aborts through ernic_log_fatal() if there is none.
 */
void ernic_u32_map_init(ErnicU32Map *map, ErnicMapDestroyFn value_destroy);

/**
 * Remove every entry, passing each value to the map's destroy function
 *
 * @param map the map
 *
 * The map is left empty, as ernic_u32_map_init() leaves it, so it may be
 * used again.
 */
void ernic_u32_map_destroy(ErnicU32Map *map);

/**
 * Map @p key to @p value
 *
 * @param map   the map
 * @param key   the key
 * @param value the value, which may be NULL
 *
 * A value @p key already had goes to the map's destroy function.  Aborts
 * through ernic_log_fatal() when out of memory.
 */
void ernic_u32_map_insert(ErnicU32Map *map, uint32_t key, void *value);

/**
 * Look up a key
 *
 * @param map the map
 * @param key the key
 *
 * Returns the value of @p key, or NULL if the map doesn't hold it.  Use
 * ernic_u32_map_contains() where a value may be NULL.
 */
void *ernic_u32_map_lookup(const ErnicU32Map *map, uint32_t key);

/**
 * Whether the map holds a key
 *
 * @param map the map
 * @param key the key
 */
bool ernic_u32_map_contains(const ErnicU32Map *map, uint32_t key);

/**
 * Remove a key, passing its value to the map's destroy function
 *
 * @param map the map
 * @param key the key
 *
 * Returns whether the map held @p key.
 */
bool ernic_u32_map_remove(ErnicU32Map *map, uint32_t key);

/**
 * How many keys the map holds
 *
 * @param map the map
 */
size_t ernic_u32_map_size(const ErnicU32Map *map);

/**
 * Start a walk over a map's entries, in no particular order
 *
 * @param iter the walk
 * @param map  the map
 *
 * The map must not change until the walk ends, though the values it points
 * to may.
 */
void ernic_u32_map_iter_init(ErnicU32MapIter *iter, const ErnicU32Map *map);

/**
 * Step to the walk's next entry
 *
 * @param iter  the walk
 * @param key   where to store the entry's key
 * @param value where to store the entry's value
 *
 * Returns false, storing nothing, once every entry has been visited.
 */
bool ernic_u32_map_iter_next(ErnicU32MapIter *iter, uint32_t *key,
                             void **value);

#endif /* ERNIC_MAP_H */
