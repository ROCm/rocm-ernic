/*
 * nvmeof_kv.h — the key-value store behind an NVMe KV namespace
 *
 * A bounded map from a 1..16 byte key to a value, with no I/O of its own:
 * the caller moves bytes in and out through the target's DMA callbacks, so
 * this is testable with a plain byte array and runs under CTest without a
 * VM, RDMA hardware, or root — the same split nvmeof_target.h describes.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NVMEOF_KV_H
#define NVMEOF_KV_H

#include <stdbool.h>
#include <stdint.h>

/* The NVM Express Key Value Command Set caps a key at 16 bytes. */
#define NVMEOF_KV_KEY_MAX 16

/*
 * Defaults and bounds for a KV namespace.  The key ceiling is what keeps a
 * `keys=` option from asking for a table the host cannot afford: the store
 * allocates two slots per key up front.
 */
#define NVMEOF_KV_DEFAULT_KEYS  4096
#define NVMEOF_KV_DEFAULT_VALUE (1024U * 1024U)
#define NVMEOF_KV_MAX_KEYS      (1024U * 1024U)

struct nvmeof_kv;

/*
 * A key as it arrives on the wire: the bytes and how many of them are
 * meaningful.  Passing the length separately rather than NUL-terminating
 * matters, because a key is an arbitrary octet string and may contain zero
 * bytes anywhere in it.
 */
struct nvmeof_kv_key {
    uint8_t bytes[NVMEOF_KV_KEY_MAX];
    uint8_t len;
};

/*
 * Create a store holding at most max_keys keys and max_bytes of value data
 * in total.  Whichever runs out first is what stops the next store, which
 * is the same rule the S3 target applies to objects.  Returns NULL when
 * either bound is zero or the allocation fails.
 */
struct nvmeof_kv *nvmeof_kv_create(uint32_t max_keys, uint64_t max_bytes);
void nvmeof_kv_destroy(struct nvmeof_kv *kv);

/* True when the key is present. */
bool nvmeof_kv_exists(const struct nvmeof_kv *kv,
                      const struct nvmeof_kv_key *key);

/*
 * Look a key up.  On success *value and *len describe memory the store
 * owns, valid until the next call that modifies this key.  Returns false
 * when the key is absent.
 */
bool nvmeof_kv_get(const struct nvmeof_kv *kv, const struct nvmeof_kv_key *key,
                   const uint8_t **value, uint32_t *len);

/*
 * Replace or create the value for a key.  The bytes are copied, so the
 * caller's buffer can be reused immediately.  Returns false when the store
 * is full — out of key slots or out of capacity — after leaving any
 * previous value for the key untouched.
 */
bool nvmeof_kv_put(struct nvmeof_kv *kv, const struct nvmeof_kv_key *key,
                   const uint8_t *value, uint32_t len);

/* Remove a key.  Returns false when it was not there. */
bool nvmeof_kv_delete(struct nvmeof_kv *kv, const struct nvmeof_kv_key *key);

/*
 * Walk the keys in unspecified order, starting from *iter, which must be
 * zero on the first call.  Returns false when there are no more.  Keys
 * added or removed during a walk may or may not be seen; the walk stays
 * within the store either way.
 */
bool nvmeof_kv_next(const struct nvmeof_kv *kv, uint32_t *iter,
                    struct nvmeof_kv_key *key);

/* How many keys are live, and how many value bytes they hold. */
uint32_t nvmeof_kv_count(const struct nvmeof_kv *kv);
uint64_t nvmeof_kv_used(const struct nvmeof_kv *kv);

#endif /* NVMEOF_KV_H */
