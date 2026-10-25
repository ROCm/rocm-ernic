/*
 * p4_engine.h — the programmable match-action stage on the emulated wire
 *
 * The device rocm-ernic emulates is, in silicon, a P4-programmable DPU: its
 * datapath is a match-action pipeline loaded at run time rather than a fixed
 * function.  This module is the emulator's equivalent.  It sits between the
 * guest's Ethernet queues and the host network backend and decides, for every
 * frame in either direction, where the frame goes and what it looks like when
 * it gets there.
 *
 * The program is a bmv2 JSON file -- the target-independent artifact that
 * `p4c --target bmv2 --arch v1model` emits for any P4 program.  Nothing here
 * links bmv2 itself; the JSON is the interface.  See docs/p4.rst for the
 * subset of the format that is supported and for where this deliberately
 * differs from bmv2.
 *
 * Nothing in this file knows about vfio-user, queues or guest memory: the
 * engine takes a frame and returns a verdict, so the whole thing runs under
 * CTest with no VM.  src/p4/p4_port.h is the part that knows about the
 * emulated NIC.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef P4_ENGINE_H
#define P4_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct p4_engine;

/*
 * Logical ports.  The pipeline sees the emulated NIC as a two-port device,
 * which is what the guest and the host network respectively are.  A P4
 * program's ingress_port and egress_spec metadata carry these values.
 */
#define P4_PORT_HOST   0u /* the guest's Ethernet queues */
#define P4_PORT_UPLINK 1u /* the host network backend (TAP) */
#define P4_PORT_COUNT  2u

/*
 * Largest frame the engine will accept or produce.
 *
 * This is the emulated NIC's frame buffer size, not a round jumbo number:
 * p4_port.c claims every frame unconditionally, so anything the engine
 * refuses here is a frame the guest sent and nobody forwards.  At 9216
 * that silently dropped every frame between 9217 bytes and the 9582-byte
 * MTU the guest is allowed to configure -- and only when --p4 was on, so
 * it read as the engine breaking jumbo frames rather than as a limit.
 *
 * p4_port.c asserts this covers IONIC_ETH_NET_MTU_MAX, which is where the
 * two numbers would otherwise drift apart again.  The engine itself keeps
 * no dependency on the emulator's headers.
 */
#define P4_FRAME_MAX 9600u

/*
 * Largest program the engine will accept.  A bmv2 JSON for a pipeline the
 * size of DASH is a few hundred kilobytes; this is well clear of that and
 * bounds what a guest can make the server allocate.
 */
#define P4_PROGRAM_MAX (4u * 1024u * 1024u)

enum p4_verdict {
    P4_VERDICT_DROP = 0,
    P4_VERDICT_FORWARD,
};

struct p4_result {
    enum p4_verdict verdict;
    uint32_t egress_port;
    size_t len; /* bytes written to the output buffer; 0 when dropped */
};

struct p4_stats {
    uint64_t frames_in[P4_PORT_COUNT];
    uint64_t frames_out[P4_PORT_COUNT];
    uint64_t frames_dropped;
    uint64_t frames_too_big;
    uint64_t parse_errors;
};

struct p4_engine *p4_engine_create(void);
void p4_engine_destroy(struct p4_engine *eng);

/* True once a program has been loaded and accepted. */
bool p4_engine_has_program(const struct p4_engine *eng);

/*
 * Loading a program.
 *
 * A program arrives in pieces, because it is far larger than any single
 * command carries, and every piece comes from outside the process -- in the
 * guest-facing case, from the guest itself.  The transport is separate from
 * this: whatever carries the bytes calls these, and nothing here knows how
 * they arrived.
 *
 * The sequence is begin, then chunks covering [0, total) exactly once each
 * in increasing order, then commit.  Anything else fails.  Pieces are
 * required to arrive in order and without gaps rather than being reassembled
 * from arbitrary offsets: a sparse accumulator would have to track which
 * ranges had been filled, and the only caller that needs it is one trying to
 * find a hole in the bookkeeping.
 *
 * A begin while another load is in progress abandons that one.  A failed
 * chunk or commit abandons the load, so a caller cannot retry into a
 * half-filled buffer.
 */
int p4_engine_load_begin(struct p4_engine *eng, size_t total_len);
int p4_engine_load_chunk(struct p4_engine *eng, uint64_t offset,
                         const void *data, size_t len);
int p4_engine_load_commit(struct p4_engine *eng);
void p4_engine_load_abort(struct p4_engine *eng);

struct p4_update;
struct p4_rdma_meta;
struct p4_extern_ops;

/*
 * Run the loaded program over an RDMA operation.
 *
 * Nothing is parsed and nothing is deparsed: the pipeline sees the
 * operation's metadata and the program's own, and decides only whether to
 * suppress it.  *@suppress is set accordingly.
 *
 * @ops is how an action's extern call reaches the operation's own memory
 * regions; it may be NULL, which makes any such call fail the operation.
 *
 * Returns 0, -ENOENT when no program is loaded or the loaded one declares
 * no rdma_metadata and so was never meant for this path, or another
 * negative errno when the pipeline could not be run.  A caller that gets
 * an error must fail the operation rather than carry on: the pipeline is
 * the policy, and proceeding past a failed one bypasses it.
 *
 * *@suppress is set on an error too, to whether the program had marked
 * the operation dropped before the pipeline stopped.  A caller that
 * treats some error as "no decision" (an extern declining a request that
 * is not its kind) must still honour a drop made ahead of it.
 */
int p4_engine_process_rdma(struct p4_engine *eng,
                           const struct p4_extern_ops *ops,
                           const struct p4_rdma_meta *meta, bool *suppress);

/*
 * Install, change or remove one table entry.
 *
 * The record is shaped like a P4Runtime update, and is the single path by
 * which any control plane changes a table: a network-facing client and the
 * guest's own device commands both produce one of these rather than each
 * having its own way in.  @err, when given, receives a one-line reason.
 *
 * Returns 0, or a negative errno -- -ENOENT when no program is loaded.
 */
int p4_engine_update(struct p4_engine *eng, const struct p4_update *up,
                     char *err, size_t err_len);

/*
 * Why the last load was refused, or NULL if none has been.  The string
 * names the offending construct where it can, so a caller can tell a user
 * which P4 feature they used rather than that the file was bad.
 */
const char *p4_engine_load_error(const struct p4_engine *eng);

/* Bytes accepted so far, and the total expected; both 0 when idle. */
void p4_engine_load_progress(const struct p4_engine *eng, size_t *got,
                             size_t *total);

/*
 * Run one frame through the pipeline.
 *
 * @in_port must be below P4_PORT_COUNT.  @out receives the frame as the
 * pipeline left it and must be at least @len bytes, and in general up to
 * P4_FRAME_MAX, since a program may add headers.  @res is filled in on
 * success and is only meaningful then.
 *
 * Returns 0, or a negative errno when the frame could not be processed at
 * all -- which is distinct from the pipeline deciding to drop it.
 */
int p4_engine_process(struct p4_engine *eng, uint32_t in_port,
                      const void *frame, size_t len, void *out, size_t out_cap,
                      struct p4_result *res);

void p4_engine_get_stats(const struct p4_engine *eng, struct p4_stats *out);

#endif /* P4_ENGINE_H */
