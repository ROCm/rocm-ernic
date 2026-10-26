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

#include <stdio.h>

/*
 * Set the "nic_profile" line of the stats file's Instance block: the NIC
 * profile chain and hash.  NULL clears it.
 */
void pvrdma_stats_set_nic_profile(const char *profile);

/* Writes one section of the stats file to @fp. */
typedef void (*pvrdma_stats_section_fn)(FILE *fp, void *opaque);

/*
 * Add a section, written after "Device Statistics", for counters that live
 * outside PVRDMADev (the ionic NIC's LIF block).  NULL @fn removes it.
 */
void pvrdma_stats_set_section(pvrdma_stats_section_fn fn, void *opaque);

#endif /* PVRDMA_STATS_H */
