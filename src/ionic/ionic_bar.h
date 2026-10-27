/*
 * ionic_bar.h - the emulated ionic device's BAR layout
 *
 * Where the guest driver finds each register block, the MSI-X table and the
 * doorbell pages.  The server sizes the BARs from this and serves the MSI-X
 * table; ionic_eth_emu.c serves the register window and the doorbells.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef IONIC_BAR_H
#define IONIC_BAR_H

/* ---------------------------------------------------------------------------
 * ionic BAR layout.  Checked against upstream v7.2.4:
 * drivers/net/ethernet/pensando/ionic/{ionic_if.h,ionic_dev.c,ionic_bus_pci.c}
 *
 * ionic_map_bars() walks PCI BARs 0..5, skips any without IORESOURCE_MEM,
 * and compacts what is left into ionic->bars[].  So the driver's indices
 * are positional, not PCI BAR numbers:
 *   bars[0] = register BAR   -- ionic_dev_setup() requires len >= 0x8000
 *   bars[1] = doorbell BAR   (IONIC_PCI_BAR_DBELL)
 *   bars[2] = CMB, optional  (IONIC_PCI_BAR_CMB)
 *
 * Our BAR0 is 64-bit, so it consumes PCI BAR0+BAR1 and the guest sees our
 * PCI BAR2 as bars[1].  We expose no third MEM BAR, so the driver takes
 * the "no CMB" path.
 *
 * BAR0 map.  The first 32 KB is the ionic register window, laid out
 * exactly as ionic_dev_setup() expects.  MSI-X cannot live at offset 0
 * (that is the DEVI signature the driver probes) or at 0x2000 (intr_ctrl),
 * so the table and PBA sit above the register window:
 *
 *   0x0000  dev_info_regs   (signature, fw_status, fw_heartbeat, ...)
 *   0x0800  dev_cmd_regs    (doorbell, done, cmd, comp, data)
 *   0x1000  intr_status
 *   0x2000  intr_ctrl       (32 B per vector)
 *   0x8000  MSI-X table     <- above IONIC_BAR0_REGS_SIZE, invisible to ionic
 *   0xa000  MSI-X PBA
 *   0x10000 end
 *
 * BAR2: doorbell pages -- kernel page at index kern_pid, user pages at
 * higher indices via mmap.
 * ---------------------------------------------------------------------------
 */
/* The register window's blocks */
#define IONIC_BAR0_DEV_INFO_REGS_OFFSET 0x0000u
#define IONIC_BAR0_DEV_CMD_REGS_OFFSET  0x0800u
#define IONIC_BAR0_INTR_STATUS_OFFSET   0x1000u
#define IONIC_BAR0_INTR_CTRL_OFFSET     0x2000u

#define IONIC_BAR0_REGS_SIZE  0x8000u  /* 32 KB ionic register window */
#define IONIC_BAR0_MSIX_TABLE 0x8000u  /* MSI-X table, above the regs */
#define IONIC_BAR0_MSIX_PBA   0xa000u  /* MSI-X PBA                   */
#define IONIC_BAR0_TOTAL_SIZE 0x10000u /* 64 KB total BAR0            */

#define IONIC_BAR2_DB_SIZE (4 * 1024 * 1024) /* 4 MB doorbell pages */
#define IONIC_DB_PAGE_SIZE 4096              /* one 4K page per LIF */
#define IONIC_KERN_PID     0                 /* kernel doorbell page */

/* ionic MSI-X vectors.  ionic_lif_size() budgets 1 (adminq) + one per Tx/Rx
 * queue pair + one per RDMA EQ, and ionic_create_rdma_admin() hard-fails with
 * -EINVAL below IONIC_EQ_COUNT_MIN = 4 EQs, so a 4-vector table cannot carry
 * both halves of the driver.  This is the most any NIC profile may ask for:
 * 1 + 64 queue pairs + 32 EQs = 97 fits.  The table actually set up is the
 * profile's queues.intr_vectors. */
#define IONIC_MSIX_MAX_VECTORS 128

/* An MSI-X table entry is 16 bytes and the PBA one bit per vector, in
 * qwords; both must fit where the capability says they are. */
_Static_assert(IONIC_BAR0_MSIX_TABLE >= IONIC_BAR0_REGS_SIZE,
               "the MSI-X table must sit above the ionic register window");
_Static_assert(IONIC_BAR0_MSIX_TABLE + IONIC_MSIX_MAX_VECTORS * 16u <=
                   IONIC_BAR0_MSIX_PBA,
               "the MSI-X table for every vector must end before the PBA");
_Static_assert(IONIC_BAR0_MSIX_PBA +
                       (IONIC_MSIX_MAX_VECTORS + 63u) / 64u * 8u <=
                   IONIC_BAR0_TOTAL_SIZE,
               "the MSI-X PBA for every vector must fit in BAR0");

#endif /* IONIC_BAR_H */
