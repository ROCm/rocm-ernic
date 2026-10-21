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

/* Largest frame the engine will accept or produce. */
#define P4_FRAME_MAX 9216u

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
