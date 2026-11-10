/*
 * fake_pci_config.h - vfu_pci_get_config_space() for unit tests that stand
 * in for libvfio-user
 *
 * The device may DMA and raise interrupts only while the guest has Bus
 * Master Enable set, which ernic_dma_bus_master() reads from the function's
 * config space. This one has it set, as a driver leaves it after
 * pci_set_master(), so code under test runs as it would for a bound
 * driver; fake_pci_config_set_bus_master() clears it. Tests that link the
 * real libvfio-user include it too, since their context is NULL. It
 * defines a function, so include it once, in one translation unit.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef FAKE_PCI_CONFIG_H
#define FAKE_PCI_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include <linux/pci_regs.h>
#include <vfio-user/libvfio-user.h>
#include <vfio-user/pci_defs.h>

static vfu_pci_config_space_t fake_pci_config = {
    .raw = {[PCI_COMMAND] = PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER},
};

vfu_pci_config_space_t *vfu_pci_get_config_space(vfu_ctx_t *vfu_ctx)
{
    (void)vfu_ctx;
    return &fake_pci_config;
}

/* Set or clear Bus Master Enable, as the guest's Command register write
 * would. PCI_COMMAND_MASTER is in the register's low byte. */
static inline void fake_pci_config_set_bus_master(bool on)
{
    if (on) {
        fake_pci_config.raw[PCI_COMMAND] |= PCI_COMMAND_MASTER;
    } else {
        fake_pci_config.raw[PCI_COMMAND] &= (uint8_t)~PCI_COMMAND_MASTER;
    }
}

#endif /* FAKE_PCI_CONFIG_H */
