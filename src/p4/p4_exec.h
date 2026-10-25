/*
 * p4_exec.h — running a loaded bmv2 program over one frame
 *
 * Parse, walk the pipeline, deparse.  The program is a flat blob of small
 * integer indices resolved at load, so nothing here re-validates a
 * reference: every index it follows was checked once, when the program was
 * accepted.  What it does check on every packet is the frame -- lengths,
 * offsets and widths -- because the frame comes from the guest or the wire
 * and the program says nothing about how long it will be.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef P4_EXEC_H
#define P4_EXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Total storage for extracted headers and metadata, in bytes. */
#define P4_PHV_BYTES 1024


/*
 * The ingress_port an RDMA operation arrives on.
 *
 * A VALUE, never an index.  P4_PORT_COUNT stays 2: it bounds the egress
 * arrays and the drop check in p4_engine_process(), and raising it would
 * turn an Ethernet frame whose program set egress_spec to 2 from a drop
 * into a send out of the host backend.
 */
#define P4_PORT_RDMA 2u

/* One RDMA operation, as the pipeline sees it. */
struct p4_rdma_meta {
    uint8_t opcode;
    uint32_t qp_num;
    uint32_t length;
    uint32_t imm_data;
    uint32_t rkey;
};

struct p4_value;

/*
 * How the pipeline calls out to an extern.
 *
 * The architecture holds the operands and the program chooses the verb.
 * A value a P4 program computed is not a capability: letting a table
 * entry supply a key and an address would make the control plane an
 * addressing authority over guest memory, which is the opposite of what
 * the memory region table is for.  So @ctx carries the operation's own
 * regions, the pipeline cannot see it, and the only thing crossing from
 * the program is @args -- the operation selector.
 *
 * @call returns 0, or a negative errno that fails the packet.
 */
struct p4_extern_ops {
    int (*call)(void *ctx, uint8_t type, uint8_t method,
                const struct p4_value *args, uint8_t nargs);
    void *ctx;
};

/* What running the pipeline decided. */
struct p4_exec_result {
    bool dropped;
    /*
     * True when the program assigned egress_spec.  False means it ran to
     * completion without choosing a port, which the frame path treats as
     * "no opinion" rather than as a request for port 0.
     */
    bool egress_assigned;
    uint32_t egress_spec;
    size_t len; /* bytes written to the output buffer */
};

/*
 * Run @prog over the @len bytes at @frame, arriving on @ingress_port, and
 * write the deparsed result to @out.
 *
 * Returns 0, or a negative errno when the frame could not be run at all --
 * which is distinct from the program deciding to drop it, reported in
 * @res.  -EMSGSIZE means the frame did not hold the headers the parser
 * wanted, or the result did not fit @out_cap.
 */
struct p4_entries;
struct p4_program;

int p4_exec_run(const struct p4_program *prog, const struct p4_entries *ent,
                uint32_t ingress_port, const void *frame, size_t len, void *out,
                size_t out_cap, struct p4_exec_result *res);

/*
 * Run @prog over an RDMA operation rather than a frame.
 *
 * There is nothing to parse -- an RDMA payload is application data, not a
 * header stack -- so no parser runs, no deparser runs, and the only state
 * the pipeline sees is @meta plus whatever the program's own metadata
 * headers hold.  Non-metadata headers stay invalid, so `valid(ethernet)`
 * is false and reads of its fields give zero, which is what lets one
 * program serve both planes.
 *
 * The verdict is @res->dropped alone.  egress_spec is meaningless here and
 * is deliberately not read: it is zero in a freshly initialised packet,
 * zero is a legitimate port, and a table with no entries runs no action at
 * all -- so keying the verdict on it would suppress every operation on a
 * device whose tables are empty.
 *
 * @ops is how an action's extern call reaches the operation's regions.
 * Passing NULL makes any such call fail the operation, which is what the
 * frame path does: an extern's operands come from a work request, and a
 * frame has none.
 *
 * Returns 0, -EINVAL on a bad argument, -ENOENT when the program declares
 * no rdma_metadata and so was never meant for this path, or the negative
 * errno with which the pipeline stopped -- an extern's own, for one.
 *
 * When the pipeline stops, @res->dropped still says whether the program
 * had marked the operation dropped before it did.  That drop was decided
 * and stands; a caller that tolerates the error must not let the
 * operation through over it.  Every other field is zero.
 */
int p4_exec_run_meta(const struct p4_program *prog,
                     const struct p4_entries *ent,
                     const struct p4_extern_ops *ops,
                     const struct p4_rdma_meta *meta,
                     struct p4_exec_result *res);

#endif /* P4_EXEC_H */
