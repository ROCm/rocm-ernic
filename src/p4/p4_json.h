/*
 * p4_json.h — loading a bmv2 JSON program
 *
 * The program artifact is the JSON that `p4c --target bmv2 --arch v1model`
 * emits.  Nothing here links bmv2; the format is the interface.  See
 * docs/p4.rst for the subset that is supported and for where this
 * deliberately differs.
 *
 * Everything this parses is untrusted.  The file may come from a guest, so
 * the walker treats a malformed, oversized, cyclic or merely unexpected
 * document as a rejection rather than something to work around: there is no
 * clamping, no truncation and no guessing.  An unsupported construct is
 * reported by name, so a user learns which feature they used rather than
 * being told the file is bad.
 *
 * Keys the format documentation does not mention are ignored rather than
 * rejected.  That is deliberate and load-bearing: p4c emits `program`,
 * `source_info`, `action_ids` and `force_arith`, none of which appear in
 * bmv2's own JSON_format.md, so a loader strict about unknown keys rejects
 * every real artifact.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef P4_JSON_H
#define P4_JSON_H

#include <stddef.h>

struct p4_program;

/*
 * Depth limit handed to json-c's tokener.  32 is json-c's own default.  The
 * budget: roughly seven levels of document structure before an expression
 * starts, plus two JSON levels for each level of expression nesting, so a
 * P4_MAX_EXPR_DEPTH of 8 needs about 23.  The same limit bounds json-c's
 * recursive free of the tree it built.
 */
#define P4_JSON_MAX_DEPTH 32

/*
 * The depth is limited, but the breadth is not, and json-c spends far more
 * on a value than the value takes as text: measured on json-c 0.18, `{}`
 * costs about 790 bytes of heap (an object starts with a 16-entry hash
 * table), `[]` about 150, and a number, string or object key 70 to 105. A
 * P4_PROGRAM_MAX document of `{},{},...` would take a gigabyte, all
 * allocated on the thread that services the vfio-user poll loop.
 *
 * So before json-c sees a document, the loader weighs what json-c would
 * build from it, in these units of about 70 to 100 bytes of heap, and
 * refuses one over P4_JSON_MAX_COST. That caps the tree at about 50 MiB,
 * and the parse at tens of milliseconds, whatever the document's shape.
 * The p4c artifacts in tests/data/p4/ take a unit for every 8 to 13 bytes
 * of text as p4c indents it, or 3.5 to 5 minified, so the budget admits
 * one of about 4 MiB as p4c writes it, or at least 1.75 MiB minified.
 */
#define P4_JSON_COST_OBJECT 8u
#define P4_JSON_COST_ARRAY  2u
#define P4_JSON_COST_OTHER  1u /* a number, string, literal or object key */
#define P4_JSON_MAX_COST    (512u * 1024u)

/*
 * Parse @len bytes of bmv2 JSON at @buf into a newly allocated program,
 * stored in *@out on success.
 *
 * The program is heap allocated and the caller owns it: it runs to
 * megabytes, which is not something to put on the stack of a function on
 * the thread that also services the vfio-user poll loop.  On failure *@out
 * is untouched and nothing is allocated.
 *
 * @err, when not NULL, receives a one-line description of the first thing
 * that was wrong, NUL terminated and at most @err_len bytes including the
 * terminator.  It names the offending construct where it can.
 *
 * Returns 0, or a negative errno: -EINVAL for a document this will not
 * accept, including one over P4_JSON_MAX_COST, -ENOMEM, -ENOSYS for a
 * construct that is understood but not implemented.
 */
int p4_json_parse(const void *buf, size_t len, struct p4_program **out,
                  char *err, size_t err_len);

/* Release a program returned by p4_json_parse().  Safe with NULL. */
void p4_json_free(struct p4_program *prog);

#endif /* P4_JSON_H */
