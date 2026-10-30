/*
 * NIC profiles for the ionic emulator.
 *
 * A profile is the shape of the emulated NIC: its identity (ASIC type and
 * revision, firmware version) and its link (speed, transceiver, largest
 * MTU).  Profiles are ini files read with glib's GKeyFile:
 *
 *   [identity]
 *   fw_version = 1.8.0-E-32
 *
 *   [link]
 *   speed_mbps = 400000
 *   xcvr = fiber
 *
 *   [queues]
 *   eth = 16
 *   intr_vectors = auto
 *
 *   [stats]
 *   interval_ms = 100
 *
 * The built-in "compat" profile is today's device, byte for byte, and is
 * always the base.  Each --nic-profile layers a built-in profile (a bare
 * name) or a file (a name with a '/' or ending in ".ini") on top, key by
 * key, later layers winning.  Validation is strict: an unknown section or
 * key, a bad type or a value out of range names the source, line and key,
 * so a typo can never silently leave a default in place.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef IONIC_PROFILE_H
#define IONIC_PROFILE_H

#include <stddef.h>
#include <stdint.h>

/* IONIC_ASIC_TYPE_* in the kernel's ionic_if.h. */
enum ionic_profile_asic {
    IONIC_PROFILE_ASIC_NONE = 0,
    IONIC_PROFILE_ASIC_CAPRI = 1,
    IONIC_PROFILE_ASIC_ELBA = 2,
    IONIC_PROFILE_ASIC_GIGLIO = 3,
    IONIC_PROFILE_ASIC_SALINA = 4,
};

/* The transceiver port_info reports; it decides ethtool's port type. */
enum ionic_profile_xcvr {
    IONIC_PROFILE_XCVR_COPPER, /* direct-attach copper: PORT_DA */
    IONIC_PROFILE_XCVR_FIBER,  /* an optical module: PORT_FIBRE */
    IONIC_PROFILE_XCVR_NONE,   /* no module: PORT_NONE */
};

/* dev_info_regs fw_version is char[32], NUL-terminated. */
#define IONIC_PROFILE_FW_VERSION_MAX 31U

/*
 * link.mtu_max is the largest MTU the guest may set.  The driver derives it
 * from the identity max_frame_size as max_frame_size - VLAN_ETH_HLEN (18),
 * and the emulator's frame buffer (IONIC_ETH_NET_MTU_MAX, 9600 bytes) caps
 * the frame.
 */
#define IONIC_PROFILE_FRAME_OVERHEAD 18U
#define IONIC_PROFILE_MTU_MIN        1500U
#define IONIC_PROFILE_MTU_MAX        (9600U - IONIC_PROFILE_FRAME_OVERHEAD)

/*
 * stats.interval_ms is how often the device rewrites the statistics blocks
 * in guest memory.  The driver never asks for them, so this is also how
 * stale `ip -s link` may be.  --stats-interval-ms overrides it.
 */
#define IONIC_PROFILE_STATS_INTERVAL_MIN 10U
#define IONIC_PROFILE_STATS_INTERVAL_MAX 60000U

/*
 * queues.eth is the RX and TX queue count the LIF identity advertises; the
 * guest uses min(eth, online vCPUs) of them.  --eth-queues overrides it.
 */
#define IONIC_PROFILE_ETH_QUEUES_MIN 1U
#define IONIC_PROFILE_ETH_QUEUES_MAX 64U

/*
 * queues.rdma_eqs is the RDMA EQ count.  The RDMA driver fails below 4
 * (IONIC_EQ_COUNT_MIN), and the emulator keeps state for at most 32.
 */
#define IONIC_PROFILE_RDMA_EQS_MIN 4U
#define IONIC_PROFILE_RDMA_EQS_MAX 32U

/*
 * queues.intr_vectors is the MSI-X table size and the identity nintrs.  The
 * driver needs 1 (adminq and notifyq) + one per queue pair + one per EQ;
 * "auto" (stored as 0) is exactly that for every queue and EQ.
 */
#define IONIC_PROFILE_INTR_VECTORS_AUTO 0U
#define IONIC_PROFILE_INTR_VECTORS_MIN  2U
#define IONIC_PROFILE_INTR_VECTORS_MAX  128U

/* The built-in profile names and files that formed a profile, in order. */
#define IONIC_PROFILE_CHAIN_MAX 512U

/* A profile file larger than this is refused rather than read. */
#define IONIC_PROFILE_FILE_MAX ((size_t)1024 * 1024)

struct ionic_profile {
    struct {
        uint8_t asic_type; /* enum ionic_profile_asic */
        uint8_t asic_rev;
        char fw_version[IONIC_PROFILE_FW_VERSION_MAX + 1];
    } identity;
    struct {
        uint32_t speed_mbps; /* one of the IONIC_SPEED_* values */
        uint8_t xcvr;        /* enum ionic_profile_xcvr */
        uint32_t mtu_max;
    } link;
    struct {
        uint32_t eth;
        uint32_t rdma_eqs;
        uint32_t intr_vectors; /* or IONIC_PROFILE_INTR_VECTORS_AUTO */
    } queues;
    struct {
        uint32_t interval_ms;
    } stats;
    char chain[IONIC_PROFILE_CHAIN_MAX];
};

/* The compat profile: today's emulated device.  Its chain is "compat". */
void ionic_profile_defaults(struct ionic_profile *p);

/*
 * Layer ini @text (@len bytes, need not be NUL-terminated) from @source
 * onto @p, key by key.  Returns 0, or -1 with "source:line: what" in @err;
 * on failure @p is unchanged.  @source is only used in messages.
 */
int ionic_profile_apply_text(struct ionic_profile *p, const char *source,
                             const char *text, size_t len, char *err,
                             size_t err_len);

/*
 * Layer the profile @spec names onto @p and append it to the chain: a
 * built-in profile for a bare name, else the file at that path.  Returns 0,
 * or -1 with a message in @err; on failure @p is unchanged.
 */
int ionic_profile_apply(struct ionic_profile *p, const char *spec, char *err,
                        size_t err_len);

/*
 * Write @p as an ini profile that ionic_profile_apply_text() reads back to
 * the same values: a comment with the chain and hash, then every key.
 * Returns the length it needed, as snprintf() does.
 */
int ionic_profile_format(const struct ionic_profile *p, char *buf, size_t len);

/*
 * Set stats.interval_ms from a decimal string, with the key's range check.
 * The --stats-interval-ms option uses it so the flag and the key accept the
 * same values.  Returns 0, or -1 with a message naming @where in @err.
 */
int ionic_profile_set_stats_interval(struct ionic_profile *p, const char *raw,
                                     const char *where, char *err,
                                     size_t err_len);

/*
 * Set queues.eth from a decimal string, with the key's range check, for
 * the --eth-queues option.  Returns 0, or -1 with a message naming @where
 * in @err.
 */
int ionic_profile_set_eth_queues(struct ionic_profile *p, const char *raw,
                                 const char *where, char *err, size_t err_len);

/* queues.intr_vectors with "auto" worked out: 1 + eth + rdma_eqs. */
uint32_t ionic_profile_intr_vectors(const struct ionic_profile *p);

/*
 * Rules that hold for any valid profile but deserve a warning.  Today the
 * only one: fewer vectors than 1 + queues.eth + queues.rdma_eqs, so a guest
 * with enough vCPUs halves its EQs or queues to fit.  That is allowed --
 * real NICs do it -- but never silent.  Returns 1 with the warning in @msg,
 * or 0.
 */
int ionic_profile_warning(const struct ionic_profile *p, char *msg,
                          size_t msg_len);

/*
 * A 64-bit FNV-1a hash of @p's values (not its chain), so two runs with the
 * same merged profile log the same hash however they got there.
 */
uint64_t ionic_profile_hash(const struct ionic_profile *p);

/* The ini text of built-in profile @name, or NULL. */
const char *ionic_profile_builtin(const char *name);

/* The built-in profile names, separated by ", ", for messages. */
const char *ionic_profile_builtin_names(void);

#endif /* IONIC_PROFILE_H */
