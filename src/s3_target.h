/*
 * s3_target.h — in-process S3-over-RDMA object store
 *
 * The server half of the protocol ROCm/hipObject implements on the
 * client: an S3 control plane that names objects over HTTP, and an RDMA
 * data plane that moves their bytes straight into and out of the
 * client's registered buffers.
 *
 * Like the NVMe-oF controller next door this module never touches the
 * network or guest memory itself.  It is handed a parsed HTTP request
 * and a pair of DMA callbacks and hands back an HTTP response; the
 * callbacks are what stand in for the RDMA READ and RDMA WRITE a real
 * storage server would post against the rkey in the client's token.
 * That split is what lets the whole object protocol run under CTest with
 * a byte array for guest memory, while the ionic data path in the server
 * supplies callbacks backed by vfio-user DMA.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef S3_TARGET_H
#define S3_TARGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "s3_http.h"
#include "s3_token.h"

struct s3_target;

#define S3_BUCKET_MAX 64
#define S3_KEY_MAX    512

/*
 * Data movement between a client buffer described by an RDMA token and
 * the object store.  Both return the number of bytes actually moved; a
 * short count is reported to the client as a transfer failure.  These
 * are the same two directions nvmeof_dma_ops names, for the same reason:
 * one guest owns every registration, so an rkey resolves in the same
 * table as any lkey.
 */
struct s3_dma_ops {
    uint32_t (*from_host)(void *ctx, uint32_t key, uint64_t addr, void *dst,
                          uint32_t len);
    uint32_t (*to_host)(void *ctx, uint32_t key, uint64_t addr, const void *src,
                        uint32_t len);
};

struct s3_target_cfg {
    char bucket[S3_BUCKET_MAX];
    uint64_t capacity;    /* total bytes the store will hold */
    uint32_t max_objects; /* ceiling on live keys, multipart parts aside */
    uint32_t traddr;      /* target IPv4, host byte order */
    uint16_t trsvcid;     /* HTTP port the control plane answers on */
    uint32_t max_part;    /* largest single RDMA transfer accepted */
};

struct s3_target_stats {
    uint64_t requests;
    uint64_t gets;
    uint64_t puts;
    uint64_t deletes;
    uint64_t read_bytes;  /* moved out of the store, towards the client */
    uint64_t write_bytes; /* moved into the store, from the client */
    uint64_t errors;
};

/*
 * Fill cfg with the defaults, then apply a comma-separated option string
 * (the part after "s3:"; NULL or empty leaves the defaults alone).
 * Returns false and writes a human-readable reason to err.
 */
void s3_target_cfg_defaults(struct s3_target_cfg *cfg);
bool s3_target_cfg_parse(struct s3_target_cfg *cfg, const char *opts, char *err,
                         size_t errlen);

struct s3_target *s3_target_create(const struct s3_target_cfg *cfg, char *err,
                                   size_t errlen);
void s3_target_destroy(struct s3_target *t);

const struct s3_target_cfg *s3_target_config(const struct s3_target *t);
const struct s3_target_stats *s3_target_statistics(const struct s3_target *t);

/* Live object count and bytes held, for tests and the log banner. */
unsigned s3_target_object_count(const struct s3_target *t);
uint64_t s3_target_bytes_used(const struct s3_target *t);

/*
 * Serve one request.  @resp must be uninitialised; this function calls
 * s3_http_response_init() on it and the caller frees it with
 * s3_http_response_free() once it has been serialised.
 *
 * @dma may be NULL, which serves the control plane alone: a GET then
 * returns the object in the HTTP body rather than by RDMA, which is what
 * makes the store reachable from curl for smoke tests.
 */
void s3_target_exec(struct s3_target *t, const struct s3_http_request *req,
                    const struct s3_dma_ops *dma, void *dma_ctx,
                    struct s3_http_response *resp);

/*
 * The token the target puts in x-amz-rdma-reply.  Exposed so tests can
 * assert what a client would find there without going through a request.
 */
void s3_target_peer_token(const struct s3_target *t, uint32_t rkey,
                          uint64_t addr, uint64_t length, struct s3_token *out);

/*
 * Queue pair numbers the target hands out in its reply tokens.  A client
 * that honours the peer token will transition its own QP to point at
 * one of these, so they have to be 24-bit values distinguishable from
 * the guest's own QPs, which the ionic driver allocates from zero up.
 * The NVMe-oF responder reserves 0x00c0xxxx for the same reason.
 */
#define S3_TARGET_QPN_BASE 0x00d00000u
#define S3_TARGET_QPN_MASK 0x00ff0000u

static inline bool s3_target_is_target_qpn(uint32_t qpn)
{
    return (qpn & S3_TARGET_QPN_MASK) == S3_TARGET_QPN_BASE;
}

#endif /* S3_TARGET_H */
