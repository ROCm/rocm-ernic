/*
 * Fault injection for the ionic emulator.
 *
 * A fault is a site (where in the emulator it can fire), a pattern (on
 * which of the site's eligible events it fires) and an effect (what happens
 * when it does).  The grammar follows tc netem's, so a fault reads like a
 * netem loss model:
 *
 *   stats_vals: every 100 effect fail status eio
 *   stats_vals: after 3 count 2 effect fail
 *   stats_vals: burst 5 every 100 effect fail
 *   stats_vals: sequence "1x3,0x10" effect fail
 *   eth_rx: every 10 effect drop
 *   eth_rx: burst 3 every 50 effect nobuf
 *   eth_tx: after 100 count 5 effect drop
 *
 * The sites, and the effects each takes:
 *
 *   stats_vals  fail [status <name>]  the command completes with that ionic
 *                                     status, eio by default
 *   eth_rx      drop   the frame is lost before the NIC sees it: nothing is
 *                      delivered and no LIF counter moves
 *               nobuf  the NIC drops it as if no Rx buffer were posted:
 *                      rx_queue_empty and rx_*_drop_* count it
 *   eth_tx      drop   the frame is completed to the guest but never sent:
 *                      tx_*_drop_* count it
 *
 * An eth_rx event is a frame off the TAP or the mesh, counted once even if
 * the mesh holds it back for want of a buffer; an eth_tx event is a frame
 * gathered from a Tx descriptor.
 *
 * This build has the deterministic patterns only, so a test can assert
 * exact counts.  Patterns advance only on eligible events, never on a
 * timer, so the same guest workload gives the same fires.
 *
 * Every site hook costs one branch on a NULL pointer when no fault is set.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef IONIC_FAULT_H
#define IONIC_FAULT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where a fault can fire.  Each site has one kind of eligible event. */
enum ionic_fault_site {
    IONIC_FAULT_SITE_STATS_VALS, /* a STATS_VALS admin command */
    IONIC_FAULT_SITE_ETH_RX,     /* a frame arriving for the guest */
    IONIC_FAULT_SITE_ETH_TX,     /* a frame the guest sent */
    IONIC_FAULT_SITE_COUNT,
};

enum ionic_fault_pattern_kind {
    IONIC_FAULT_PAT_EVERY,    /* events N, 2N, 3N, ... */
    IONIC_FAULT_PAT_AFTER,    /* events N .. N+M-1 */
    IONIC_FAULT_PAT_BURST,    /* the first N of every M events */
    IONIC_FAULT_PAT_SEQUENCE, /* an explicit fire/pass list, looped */
};

enum ionic_fault_effect {
    IONIC_FAULT_EFFECT_FAIL,  /* complete the command with an error status */
    IONIC_FAULT_EFFECT_DROP,  /* lose the frame on the wire */
    IONIC_FAULT_EFFECT_NOBUF, /* drop it as if no Rx buffer were posted */
};

/* The longest sequence a pattern may expand to, in events. */
#define IONIC_FAULT_SEQ_MAX 65536u

struct ionic_fault {
    enum ionic_fault_site site;
    enum ionic_fault_pattern_kind kind;
    uint64_t n;   /* every N; after N; burst N */
    uint64_t m;   /* after .. count M; burst .. every M */
    uint8_t *seq; /* SEQUENCE: one byte per event, 1 = fire */
    uint32_t seq_len;
    enum ionic_fault_effect effect;
    uint8_t status; /* FAIL: the ionic status code to complete with; else 0 */

    uint64_t eligible; /* eligible events seen (saturates) */
    uint64_t fired;    /* events the fault fired on (saturates) */
};

struct ionic_fault_set {
    struct ionic_fault *site[IONIC_FAULT_SITE_COUNT];
};

/*
 * Parse one CLI fault, "<site>: <pattern> effect <effect> [args]", into
 * @out.  On failure returns -1 and writes a message naming the offending
 * token to @err; @out then owns nothing.  On success @out may own a
 * sequence buffer: release it with ionic_fault_clear().
 */
int ionic_fault_parse(const char *spec, struct ionic_fault *out, char *err,
                      size_t err_len);

/* Free what a parsed fault owns and zero it. */
void ionic_fault_clear(struct ionic_fault *f);

/*
 * Write @f in canonical form, which ionic_fault_parse() reads back to an
 * identical fault.  Returns the length it needed, as snprintf() does.
 */
int ionic_fault_format(const struct ionic_fault *f, char *buf, size_t len);

/*
 * Count one eligible event at @f's site and say whether the fault fires on
 * it.  @f may be NULL, which never fires.
 */
bool ionic_fault_event(struct ionic_fault *f);

/* An empty set: no site ever fires. */
struct ionic_fault_set *ionic_fault_set_new(void);
void ionic_fault_set_free(struct ionic_fault_set *set);

/*
 * Parse @spec and add it to @set.  A second fault on a site that already
 * has one is rejected.  Returns 0, or -1 with a message in @err.
 */
int ionic_fault_set_add(struct ionic_fault_set *set, const char *spec,
                        char *err, size_t err_len);

/* The fault at @site, or NULL.  @set may be NULL. */
struct ionic_fault *ionic_fault_set_get(const struct ionic_fault_set *set,
                                        enum ionic_fault_site site);

const char *ionic_fault_site_name(enum ionic_fault_site site);

#endif /* IONIC_FAULT_H */
