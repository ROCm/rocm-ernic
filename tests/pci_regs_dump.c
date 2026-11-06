/*
 * pci_regs_dump.c - print the linux/pci_regs.h values the Python tests use
 *
 * The Python tests read the device's config space, and take its register
 * offsets and fields from the header the server is built with rather than
 * from a copy. They run this, which prints each value they use as one
 * "NAME VALUE" line, VALUE in decimal (tests/py/ernic/pci.py).
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <linux/pci_regs.h>

struct pci_reg {
    const char *name;
    uint64_t value;
};

#define PCI_REG(name)           \
    {                           \
        #name, (uint64_t)(name) \
    }

static const struct pci_reg pci_regs[] = {
    /* The standard header */
    PCI_REG(PCI_VENDOR_ID),
    PCI_REG(PCI_DEVICE_ID),
    PCI_REG(PCI_STATUS),
    PCI_REG(PCI_STATUS_CAP_LIST),
    PCI_REG(PCI_CLASS_DEVICE),
    PCI_REG(PCI_HEADER_TYPE),
    PCI_REG(PCI_HEADER_TYPE_MASK),
    PCI_REG(PCI_HEADER_TYPE_NORMAL),
    PCI_REG(PCI_SUBSYSTEM_VENDOR_ID),
    PCI_REG(PCI_SUBSYSTEM_ID),
    PCI_REG(PCI_CAPABILITY_LIST),

    /* Capabilities */
    PCI_REG(PCI_CAP_LIST_ID),
    PCI_REG(PCI_CAP_LIST_NEXT),
    PCI_REG(PCI_CAP_ID_EXP),
    PCI_REG(PCI_CAP_ID_MSIX),

    /* The PCI Express capability */
    PCI_REG(PCI_EXP_FLAGS),
    PCI_REG(PCI_EXP_FLAGS_VERS),
    PCI_REG(PCI_EXP_FLAGS_TYPE),
    PCI_REG(PCI_EXP_TYPE_ENDPOINT),
    PCI_REG(PCI_EXP_DEVCAP),
    PCI_REG(PCI_EXP_DEVCAP_FLR),
    PCI_REG(PCI_EXP_DEVCTL),
    PCI_REG(PCI_EXP_DEVCTL_BCR_FLR),
    PCI_REG(PCI_EXP_LNKCAP),
    PCI_REG(PCI_EXP_LNKCAP_SLS),
    PCI_REG(PCI_EXP_LNKCAP_SLS_64_0GB),
    PCI_REG(PCI_EXP_LNKCAP_MLW),
    PCI_REG(PCI_EXP_LNKSTA),
    PCI_REG(PCI_EXP_LNKSTA_CLS),
    PCI_REG(PCI_EXP_LNKSTA_CLS_64_0GB),
    PCI_REG(PCI_EXP_LNKSTA_NLW),
    PCI_REG(PCI_EXP_LNKCAP2),
    PCI_REG(PCI_EXP_LNKCAP2_SLS),
    PCI_REG(PCI_EXP_LNKCAP2_SLS_2_5GB),
    PCI_REG(PCI_EXP_LNKCAP2_SLS_5_0GB),
    PCI_REG(PCI_EXP_LNKCAP2_SLS_8_0GB),
    PCI_REG(PCI_EXP_LNKCAP2_SLS_16_0GB),
    PCI_REG(PCI_EXP_LNKCAP2_SLS_32_0GB),
    PCI_REG(PCI_EXP_LNKCAP2_SLS_64_0GB),
    PCI_REG(PCI_EXP_LNKCTL2),
    PCI_REG(PCI_EXP_LNKCTL2_TLS),
    PCI_REG(PCI_EXP_LNKCTL2_TLS_64_0GT),
};

int main(void)
{
    for (size_t i = 0; i < sizeof(pci_regs) / sizeof(pci_regs[0]); i++) {
        if (printf("%s %" PRIu64 "\n", pci_regs[i].name, pci_regs[i].value) <
            0) {
            return EXIT_FAILURE;
        }
    }
    return fflush(stdout) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
