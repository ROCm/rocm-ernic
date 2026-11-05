/*
 * nvmeof_target.h — in-process NVMe over Fabrics controller
 *
 * A complete NVMe-oF controller that never touches the network or guest
 * memory itself.  The transport hands it command capsules and a pair of
 * DMA callbacks; it hands back response capsules.  That split is what
 * lets the whole controller run under CTest with a plain byte array
 * standing in for guest memory, while the ionic data path in the server
 * supplies callbacks backed by vfio-user DMA.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NVMEOF_TARGET_H
#define NVMEOF_TARGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nvmeof/nvmeof.h"

struct nvmeof_target;
struct nvmeof_queue;

/*
 * Data movement between a host (guest) buffer described by an SGL and a
 * controller-side buffer.  Both return the number of bytes actually
 * moved; a short count is reported to the host as a transfer error.
 * "key" is the SGL's rkey, zero for in-capsule descriptors.
 */
struct nvmeof_dma_ops {
    uint32_t (*from_host)(void *ctx, uint32_t key, uint64_t addr, void *dst,
                          uint32_t len);
    uint32_t (*to_host)(void *ctx, uint32_t key, uint64_t addr, const void *src,
                        uint32_t len);
};

/* The most I/O queues the controller offers.  A transport may allow fewer. */
#define NVMEOF_MAX_QUEUES 64

/*
 * The most namespaces one controller serves.  The reason to have more than
 * one here is to mix command sets, not to scale, so this is deliberately
 * small: it bounds the Identify active-namespace list to a single page.
 */
#define NVMEOF_MAX_NS 8

/*
 * One namespace.
 *
 * The command set is a property of the namespace, not of the controller.
 * NVMe 2.0 puts the Command Set Identifier in the namespace's Identify
 * descriptor, and a controller whose CAP.CSS advertises bit 6 may serve
 * block and Key Value namespaces side by side -- the host selects "all
 * supported command sets" in CC.CSS and then reads each namespace's CSI to
 * decide what it speaks.  So kv, size and block_size belong here.
 *
 * Within one namespace the two command sets remain mutually exclusive, and
 * must: a Store is opcode 01h, which the block command set reads as Write.
 * kv_max_keys and kv_max_value bound the store; "size" is the total value
 * capacity, as it is the block capacity for a block namespace.
 */
struct nvmeof_ns_cfg {
    uint32_t nsid;
    bool kv;
    char *file;          /* NULL: anonymous memory.  Never set when kv. */
    uint64_t size;       /* capacity in bytes */
    bool size_set;       /* size= was given; else an image keeps its size */
    uint32_t block_size; /* 512, 1024, 2048 or 4096; block namespaces only */
    uint32_t kv_max_keys;
    uint32_t kv_max_value;
};

struct nvmeof_target_cfg {
    char subnqn[NVMEOF_NQN_FIELD];
    char serial[NVME_ID_CTRL_SN_LEN + 1];
    char model[NVME_ID_CTRL_MN_LEN + 1];
    uint32_t traddr;     /* target IPv4, host byte order */
    uint16_t trsvcid;    /* target port */
    uint16_t max_queues; /* I/O queues offered, 1..NVMEOF_MAX_QUEUES */
    uint16_t queue_depth;

    /* At least one, in the order the option string named them. */
    struct nvmeof_ns_cfg ns[NVMEOF_MAX_NS];
    unsigned ns_count;
};

/*
 * Release what cfg_parse() allocated (the per-namespace file paths).  The
 * target keeps its own copies, so this is safe to call once create() has
 * returned, and is the only correct way to discard a parsed cfg.
 */
void nvmeof_target_cfg_free(struct nvmeof_target_cfg *cfg);

struct nvmeof_target_stats {
    uint64_t connects;
    uint64_t admin_cmds;
    uint64_t io_cmds;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t errors;
};

/*
 * Fill cfg with the defaults, then apply a comma-separated option string
 * (the part after "nvmeof:"; NULL or empty leaves the defaults alone).
 * Returns false and writes a human-readable reason to err on a bad key,
 * a bad value, or a combination that cannot work.
 */
void nvmeof_target_cfg_defaults(struct nvmeof_target_cfg *cfg);
bool nvmeof_target_cfg_parse(struct nvmeof_target_cfg *cfg, const char *opts,
                             char *err, size_t errlen);

/*
 * An image file that already has data keeps its size unless size= is set,
 * and a size= smaller than the image is refused rather than truncating it;
 * nvmeof_target_config() reports the size settled on.
 */
struct nvmeof_target *nvmeof_target_create(const struct nvmeof_target_cfg *cfg,
                                           char *err, size_t errlen);
void nvmeof_target_destroy(struct nvmeof_target *t);

const struct nvmeof_target_cfg *nvmeof_target_config(
    const struct nvmeof_target *t);
const struct nvmeof_target_stats *nvmeof_target_stats(
    const struct nvmeof_target *t);

/*
 * Queues are keyed by a transport handle -- the guest QP number in the
 * server, an arbitrary integer in tests.  open() is idempotent and
 * returns the existing queue if the handle is already live.
 */
struct nvmeof_queue *nvmeof_target_open_queue(struct nvmeof_target *t,
                                              uint32_t handle);
struct nvmeof_queue *nvmeof_target_find_queue(struct nvmeof_target *t,
                                              uint32_t handle);
void nvmeof_target_close_queue(struct nvmeof_target *t, uint32_t handle);

/* Number of queues currently open, for tests and diagnostics. */
unsigned nvmeof_target_queue_count(const struct nvmeof_target *t);

/* True once the host has completed Fabrics Connect on this queue. */
bool nvmeof_queue_connected(const struct nvmeof_queue *q);
uint16_t nvmeof_queue_id(const struct nvmeof_queue *q);

/*
 * Execute one command capsule.  capsule points at the 64-byte SQE
 * followed by any in-capsule data; rsp receives the 16-byte response.
 *
 * Returns NVMEOF_EXEC_DONE when rsp holds a response to send back,
 * NVMEOF_EXEC_HELD when the command is legitimately left outstanding
 * (Asynchronous Event Request) and nothing should be sent, or a negative
 * errno when the capsule itself is unusable.
 */
enum {
    NVMEOF_EXEC_DONE = 0,
    NVMEOF_EXEC_HELD = 1,
};

int nvmeof_queue_exec(struct nvmeof_queue *q, const void *capsule, size_t len,
                      const struct nvmeof_dma_ops *dma, void *dma_ctx,
                      void *rsp);

#endif /* NVMEOF_TARGET_H */
