/*
 * p4_table.h — match-action table entries and the units that search them
 *
 * A loaded program is immutable; its entries are not.  They arrive from a
 * control plane while packets are being forwarded, so they live here
 * rather than in the program, and the program is free to be shared.
 *
 * The update record is shaped like P4Runtime's, because that is the API
 * the ecosystem speaks and because the host-facing device commands are
 * meant to translate into the same record rather than into a second,
 * parallel way of saying the same thing.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef P4_TABLE_H
#define P4_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "p4/p4_program.h"

/*
 * Widest key field or action argument an entry may carry, in bytes; see
 * P4_MAX_ENTRY_BYTES, which the loader holds every key field and action
 * argument to.
 */
#define P4_MATCH_BYTES P4_MAX_ENTRY_BYTES

enum p4_update_type {
    P4_UPDATE_INSERT = 0,
    P4_UPDATE_MODIFY,
    P4_UPDATE_DELETE,
};

/*
 * One key field of an entry.  Which of these fields matter is decided by
 * the table's own key definition, not by the record: an exact field reads
 * @value, an LPM field also reads @prefix_bits, and a ternary field reads
 * @mask.  As in P4Runtime, @value has no bit set past an LPM field's
 * prefix or outside a ternary field's mask; and @prefix_bits is 0 in a
 * field that is not LPM.  An update that breaks any of these is refused.
 */
struct p4_match_spec {
    uint16_t len; /* significant bytes of @value and @mask */
    uint16_t prefix_bits;
    uint8_t value[P4_MATCH_BYTES];
    uint8_t mask[P4_MATCH_BYTES];
};

/* One argument of the action an entry runs. */
struct p4_param {
    uint16_t len;
    uint8_t value[P4_MATCH_BYTES];
};

/*
 * A table update.  Tables and actions are named rather than numbered: a
 * control plane that has the p4info has the names, and resolving them here
 * means a wrong name is an error with something useful in it rather than
 * an index into the wrong row.
 */
struct p4_update {
    uint8_t type;
    char table[P4_MAX_NAME + 1];
    char action[P4_MAX_NAME + 1];
    /* Higher wins.  Nonzero in a table with a ternary key, 0 in any other,
     * as in P4Runtime. */
    uint32_t priority;
    uint16_t nmatch;
    uint16_t nparams;
    struct p4_match_spec match[P4_MAX_TABLE_KEY];
    struct p4_param params[P4_MAX_RUNTIME_ARGS];
};

struct p4_entries;
struct p4_value;

/*
 * One key field of an installed entry, in the form the lookup reads: a
 * packet's field, under the program's key mask, matches when (field & @mask)
 * == @value.  Both are big-endian and right-aligned, the field's low
 * P4_MATCH_BYTES, which hold all of it: the loader refuses a key field
 * wider.  @mask is all ones for an exact field, the prefix for an LPM one,
 * and the entry's own mask for a ternary one, and @value has no bit set
 * outside it, so two entries with the same @value and @mask match the same
 * packets.
 */
struct p4_entry_key {
    uint8_t value[P4_MATCH_BYTES];
    uint8_t mask[P4_MATCH_BYTES];
};

/*
 * An entry, once installed.  Returned by a lookup so the executor can run
 * its action; the caller must not keep it across an update.
 */
struct p4_entry {
    uint32_t priority;
    uint32_t rank; /* larger wins: the LPM prefix length, or the priority */
    p4_idx_t action;
    uint16_t nparams;
    struct p4_entry_key key[P4_MAX_TABLE_KEY];
    struct p4_param params[P4_MAX_RUNTIME_ARGS];
};

/* Create a store for @prog's tables.  No entry memory is taken until an
 * entry is installed, so a program with large tables and none in them
 * costs nothing. */
struct p4_entries *p4_entries_create(const struct p4_program *prog);
void p4_entries_destroy(struct p4_entries *ent);

/*
 * Apply one update.  @err, when given, receives a one-line reason.
 * Returns 0, or a negative errno: -EINVAL for a record that does not fit
 * the table it names, -ENOENT for a delete or modify of an entry that is
 * not there, -EEXIST for an insert over one that is, -ENOSPC when the
 * table is full, -ENOMEM.
 */
int p4_entries_update(struct p4_entries *ent, const struct p4_program *prog,
                      const struct p4_update *up, char *err, size_t err_len);

/*
 * Find the entry matching @keys, or NULL.  @keys holds one value per key
 * field of the table, in order.  Ties are broken as P4 requires: the
 * longest prefix for LPM, the highest priority for ternary.  Which of two
 * matching ternary entries of the same priority wins is unspecified, and
 * can change when another entry is deleted.  The cost is in proportion to
 * the entries installed, not to the table's size.
 */
const struct p4_entry *p4_entries_lookup(const struct p4_entries *ent,
                                         const struct p4_program *prog,
                                         p4_idx_t pipe, p4_idx_t table,
                                         const struct p4_value *keys);

#endif /* P4_TABLE_H */
