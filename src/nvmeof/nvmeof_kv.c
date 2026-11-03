/*
 * nvmeof_kv.c — the key-value store behind an NVMe KV namespace
 *
 * Open addressing with linear probing over a power-of-two table.  A hash
 * table rather than a list because Exist is on the hot path for a KV cache
 * and a linear scan would make a miss cost the whole table; power-of-two so
 * the probe index is a mask rather than a division.
 *
 * Deletion uses tombstones.  Back-shift deletion would avoid them, but it
 * has to move entries the walk in nvmeof_kv_next() may already have passed,
 * and a List that silently skips a live key is a worse defect than a table
 * that needs its tombstones counted against the load factor.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "nvmeof_kv.h"

#include <stdlib.h>
#include <string.h>

/* Slot states.  EMPTY ends a probe; TOMBSTONE does not. */
enum kv_slot_state {
    KV_SLOT_EMPTY = 0,
    KV_SLOT_LIVE,
    KV_SLOT_TOMBSTONE,
};

struct kv_slot {
    struct nvmeof_kv_key key;
    uint8_t *value;
    uint32_t len;
    uint8_t state;
};

struct nvmeof_kv {
    struct kv_slot *slots;
    uint32_t mask;     /* table size - 1; table size is a power of two */
    uint32_t live;     /* keys present */
    uint32_t occupied; /* live + tombstones, for the load factor */
    uint32_t max_keys;
    uint64_t max_bytes;
    uint64_t used;
};

/*
 * FNV-1a over the key bytes.  The keyspace is a local namespace rather than
 * anything adversarial, so a short non-cryptographic hash is the right
 * trade: a collision costs a probe, not a correctness problem.
 */
static uint64_t kv_hash(const struct nvmeof_kv_key *key)
{
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (uint8_t i = 0; i < key->len; i++) {
        h = (h ^ key->bytes[i]) * UINT64_C(0x100000001b3);
    }
    /* The length is part of the identity: "ab" and "ab\0" are different
     * keys, and hashing the bytes alone would give them the same hash. */
    return (h ^ key->len) * UINT64_C(0x100000001b3);
}

static bool kv_key_eq(const struct nvmeof_kv_key *a,
                      const struct nvmeof_kv_key *b)
{
    return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

/*
 * Round up to a power of two with room to spare, so the table never runs
 * at a load factor where linear probing degrades.  Two slots per key, and
 * at least 16, which also means the mask arithmetic below has a table to
 * work on even for a tiny store.
 */
static uint32_t kv_table_size(uint32_t max_keys)
{
    uint32_t want = 16;
    while (want < max_keys * 2 && want < (UINT32_C(1) << 30)) {
        want <<= 1;
    }
    return want;
}

struct nvmeof_kv *nvmeof_kv_create(uint32_t max_keys, uint64_t max_bytes)
{
    if (max_keys == 0 || max_bytes == 0) {
        return NULL;
    }

    struct nvmeof_kv *kv = calloc(1, sizeof(*kv));
    if (kv == NULL) {
        return NULL;
    }

    uint32_t size = kv_table_size(max_keys);
    kv->slots = calloc(size, sizeof(*kv->slots));
    if (kv->slots == NULL) {
        free(kv);
        return NULL;
    }

    kv->mask = size - 1;
    kv->max_keys = max_keys;
    kv->max_bytes = max_bytes;
    return kv;
}

void nvmeof_kv_destroy(struct nvmeof_kv *kv)
{
    if (kv == NULL) {
        return;
    }
    for (uint32_t i = 0; i <= kv->mask; i++) {
        free(kv->slots[i].value);
    }
    free(kv->slots);
    free(kv);
}

/*
 * Find a key.  Returns its slot, or NULL.  The first tombstone seen is
 * reported through free_slot so a put can reuse it rather than extending
 * the probe sequence.
 */
static struct kv_slot *kv_find(const struct nvmeof_kv *kv,
                               const struct nvmeof_kv_key *key,
                               struct kv_slot **free_slot)
{
    if (free_slot != NULL) {
        *free_slot = NULL;
    }

    uint32_t idx = (uint32_t)(kv_hash(key) & kv->mask);
    for (uint32_t probe = 0; probe <= kv->mask; probe++) {
        struct kv_slot *slot = &kv->slots[(idx + probe) & kv->mask];
        if (slot->state == KV_SLOT_EMPTY) {
            if (free_slot != NULL && *free_slot == NULL) {
                *free_slot = slot;
            }
            return NULL;
        }
        if (slot->state == KV_SLOT_TOMBSTONE) {
            if (free_slot != NULL && *free_slot == NULL) {
                *free_slot = slot;
            }
            continue;
        }
        if (kv_key_eq(&slot->key, key)) {
            return slot;
        }
    }
    return NULL;
}

bool nvmeof_kv_exists(const struct nvmeof_kv *kv, const struct nvmeof_kv_key *key)
{
    return kv != NULL && key != NULL && kv_find(kv, key, NULL) != NULL;
}

bool nvmeof_kv_get(const struct nvmeof_kv *kv, const struct nvmeof_kv_key *key,
                   const uint8_t **value, uint32_t *len)
{
    if (kv == NULL || key == NULL) {
        return false;
    }
    const struct kv_slot *slot = kv_find(kv, key, NULL);
    if (slot == NULL) {
        return false;
    }
    if (value != NULL) {
        *value = slot->value;
    }
    if (len != NULL) {
        *len = slot->len;
    }
    return true;
}

bool nvmeof_kv_put(struct nvmeof_kv *kv, const struct nvmeof_kv_key *key,
                   const uint8_t *value, uint32_t len)
{
    if (kv == NULL || key == NULL || key->len == 0 ||
        key->len > NVMEOF_KV_KEY_MAX || (value == NULL && len != 0)) {
        return false;
    }

    struct kv_slot *reuse = NULL;
    struct kv_slot *slot = kv_find(kv, key, &reuse);

    /* Charge the new value and refund the old one, so replacing a value
     * with a smaller one cannot fail for want of capacity it already has. */
    uint64_t old_len = slot != NULL ? slot->len : 0;
    if (kv->used - old_len + len > kv->max_bytes) {
        return false;
    }

    uint8_t *copy = NULL;
    if (len != 0) {
        copy = malloc(len);
        if (copy == NULL) {
            return false;
        }
        memcpy(copy, value, len);
    }

    if (slot != NULL) {
        free(slot->value);
        slot->value = copy;
        slot->len = len;
        kv->used = kv->used - old_len + len;
        return true;
    }

    if (kv->live >= kv->max_keys || reuse == NULL) {
        free(copy);
        return false;
    }

    /* Reusing a tombstone does not grow the occupied count; taking a fresh
     * slot does, and that is what the load factor is measured against. */
    if (reuse->state == KV_SLOT_EMPTY) {
        kv->occupied++;
    }
    reuse->state = KV_SLOT_LIVE;
    reuse->key = *key;
    reuse->value = copy;
    reuse->len = len;
    kv->live++;
    kv->used += len;
    return true;
}

bool nvmeof_kv_delete(struct nvmeof_kv *kv, const struct nvmeof_kv_key *key)
{
    if (kv == NULL || key == NULL) {
        return false;
    }
    struct kv_slot *slot = kv_find(kv, key, NULL);
    if (slot == NULL) {
        return false;
    }

    free(slot->value);
    slot->value = NULL;
    kv->used -= slot->len;
    slot->len = 0;
    slot->state = KV_SLOT_TOMBSTONE;
    memset(&slot->key, 0, sizeof(slot->key));
    kv->live--;
    return true;
}

bool nvmeof_kv_next(const struct nvmeof_kv *kv, uint32_t *iter,
                    struct nvmeof_kv_key *key)
{
    if (kv == NULL || iter == NULL || key == NULL) {
        return false;
    }
    for (uint32_t i = *iter; i <= kv->mask; i++) {
        if (kv->slots[i].state == KV_SLOT_LIVE) {
            *key = kv->slots[i].key;
            *iter = i + 1;
            return true;
        }
    }
    *iter = kv->mask + 1;
    return false;
}

uint32_t nvmeof_kv_count(const struct nvmeof_kv *kv)
{
    return kv != NULL ? kv->live : 0;
}

uint64_t nvmeof_kv_used(const struct nvmeof_kv *kv)
{
    return kv != NULL ? kv->used : 0;
}
