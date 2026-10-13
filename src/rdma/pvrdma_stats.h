/*
 * PVRDMA stats file: state that is not part of PVRDMADev
 *
 * PVRDMADev comes from third-party/qemu and is left untouched, so values
 * the stats file shows that the QEMU device never had live here.  They are
 * process-wide, which matches the server: one device per process.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef PVRDMA_STATS_H
#define PVRDMA_STATS_H

/*
 * Set the "nic_profile" line of the stats file's Instance block: the NIC
 * profile chain and hash.  NULL clears it.  Returns 0, or -1 if out of
 * memory, in which case the line reads "(not set)".
 */
int pvrdma_stats_set_nic_profile(const char *profile);

#endif /* PVRDMA_STATS_H */
