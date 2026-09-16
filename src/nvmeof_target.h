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
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NVMEOF_TARGET_H
#define NVMEOF_TARGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nvmeof.h"

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

struct nvmeof_target_cfg {
    char subnqn[NVMEOF_NQN_FIELD];
    char serial[21];
    char model[41];
    char *file;          /* NULL: anonymous memory */
    uint64_t size;       /* namespace capacity in bytes */
    uint32_t block_size; /* 512 or 4096 */
    uint32_t nsid;
    uint32_t traddr;     /* target IPv4, host byte order */
    uint16_t trsvcid;    /* target port */
    uint16_t max_queues; /* I/O queues offered to the host */
    uint16_t queue_depth;
};

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
