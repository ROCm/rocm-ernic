/*
 * Hash maps keyed by a 32-bit number or a GID
 *
 * ErnicU32Map maps a uint32_t key, such as a QPN or a node id, to a
 * pointer; a set of numbers is one whose values are all NULL.  ErnicGidMap
 * maps a 16-byte GID to a uint32_t.  Both are typed wrappers over uthash
 * (third-party/uthash/), whose macros expand only in ernic_map.c, so the
 * code using a map never sees them.  Keys are hashed with SipHash under a
 * random per-process key, since many come from guests and peers: with an
 * unkeyed hash, keys chosen to collide would make every lookup walk one long
 * chain.
 *
 * A map may be embedded in a structure (ernic_u32_map_init()) or allocated
 * (ernic_u32_map_new()).  Either way it holds no lock.  Protect it with the
 * lock of the structure it sits in, or that points to it, as any other
 * field.
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

/* The length of an ErnicGidMap key, a GID's */
#define ERNIC_GID_MAP_KEY_LEN 16U

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
    ErnicU32Map *map;
    /* The entry the walk is at, until it is removed, or NULL */
    struct ErnicU32MapEntry *cur;
    struct ErnicU32MapEntry *next;
} ErnicU32MapIter;

/*
 * A map from a 16-byte GID to uint32_t.  Set it up with ernic_gid_map_init()
 * and touch the fields only through the functions here.
 */
typedef struct ErnicGidMap {
    /* uthash's table, NULL when empty */
    struct ErnicGidMapEntry *head;
} ErnicGidMap;

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
 * Allocate a map and set it up, as ernic_u32_map_init() does
 *
 * @param value_destroy as for ernic_u32_map_init()
 *
 * Returns the map, never NULL; free it with ernic_u32_map_free().  Aborts
 * through ernic_log_fatal() when out of memory.
 */
ErnicU32Map *ernic_u32_map_new(ErnicMapDestroyFn value_destroy);

/**
 * Empty a map from ernic_u32_map_new(), as ernic_u32_map_destroy() does,
 * and free it
 *
 * @param map the map, or NULL to do nothing
 */
void ernic_u32_map_free(ErnicU32Map *map);

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
 * Add @p key to a map used as a set
 *
 * @param map the map
 * @param key the key
 *
 * The same as ernic_u32_map_insert() with a NULL value.
 */
void ernic_u32_map_add(ErnicU32Map *map, uint32_t key);

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
 * Remove a key without passing its value to the map's destroy function
 *
 * @param map   the map
 * @param key   the key
 * @param value where to store the key's value, which is now the caller's,
 *              or NULL
 *
 * Returns whether the map held @p key, storing nothing if it didn't.
 */
bool ernic_u32_map_steal(ErnicU32Map *map, uint32_t key, void **value);

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
 * to may, except through ernic_u32_map_iter_remove() and
 * ernic_u32_map_iter_steal().
 */
void ernic_u32_map_iter_init(ErnicU32MapIter *iter, ErnicU32Map *map);

/**
 * Step to the walk's next entry
 *
 * @param iter  the walk
 * @param key   where to store the entry's key, or NULL
 * @param value where to store the entry's value, or NULL
 *
 * Returns false, storing nothing, once every entry has been visited.
 */
bool ernic_u32_map_iter_next(ErnicU32MapIter *iter, uint32_t *key,
                             void **value);

/**
 * Remove the entry the walk is at, passing its value to the map's destroy
 * function
 *
 * @param iter the walk
 *
 * The walk goes on from the next entry.  Aborts through ernic_log_fatal()
 * unless ernic_u32_map_iter_next() has just returned an entry, which no
 * call here has removed yet.
 */
void ernic_u32_map_iter_remove(ErnicU32MapIter *iter);

/**
 * Remove the entry the walk is at without passing its value to the map's
 * destroy function, as ernic_u32_map_iter_remove() does otherwise
 *
 * @param iter the walk
 */
void ernic_u32_map_iter_steal(ErnicU32MapIter *iter);

/**
 * Set up an empty map
 *
 * @param map the map
 *
 * Draws the hash key as ernic_u32_map_init() does.
 */
void ernic_gid_map_init(ErnicGidMap *map);

/**
 * Remove every entry
 *
 * @param map the map
 *
 * The map is left empty, as ernic_gid_map_init() leaves it, so it may be
 * used again.
 */
void ernic_gid_map_destroy(ErnicGidMap *map);

/**
 * Allocate a map and set it up, as ernic_gid_map_init() does
 *
 * Returns the map, never NULL; free it with ernic_gid_map_free().  Aborts
 * through ernic_log_fatal() when out of memory.
 */
ErnicGidMap *ernic_gid_map_new(void);

/**
 * Empty a map from ernic_gid_map_new() and free it
 *
 * @param map the map, or NULL to do nothing
 */
void ernic_gid_map_free(ErnicGidMap *map);

/**
 * Map @p key to @p value, replacing any value @p key had
 *
 * @param map   the map
 * @param key   the key, all ERNIC_GID_MAP_KEY_LEN bytes of which count
 * @param value the value
 *
 * Aborts through ernic_log_fatal() when out of memory.
 */
void ernic_gid_map_insert(ErnicGidMap *map,
                          const uint8_t key[ERNIC_GID_MAP_KEY_LEN],
                          uint32_t value);

/**
 * Look up a key
 *
 * @param map   the map
 * @param key   the key
 * @param value where to store the key's value
 *
 * Returns whether the map holds @p key, storing nothing if it doesn't.
 */
bool ernic_gid_map_lookup(const ErnicGidMap *map,
                          const uint8_t key[ERNIC_GID_MAP_KEY_LEN],
                          uint32_t *value);

/**
 * How many keys the map holds
 *
 * @param map the map
 */
size_t ernic_gid_map_size(const ErnicGidMap *map);

#endif /* ERNIC_MAP_H */
