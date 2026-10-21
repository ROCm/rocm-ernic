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

#include "p4_program.h"

/* Total storage for extracted headers and metadata, in bytes. */
#define P4_PHV_BYTES 1024


/* What running the pipeline decided. */
struct p4_exec_result {
    bool dropped;
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

int p4_exec_run(const struct p4_program *prog, const struct p4_entries *ent,
                uint32_t ingress_port, const void *frame, size_t len, void *out,
                size_t out_cap, struct p4_exec_result *res);

#endif /* P4_EXEC_H */
