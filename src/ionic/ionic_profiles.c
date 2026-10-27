/*
 * The built-in NIC profiles, as ini text.
 *
 * Each is parsed by the same loader as a profile file, so a built-in can't
 * use anything a file can't.  The same text ships in profiles/ for users to
 * copy and edit; tests/test_ionic_profile.c fails if the two differ.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stddef.h>
#include <string.h>

#include "ionic/ionic_profile.h"

/* profiles/compat.ini */
static const char compat_ini[] =
    "# compat: the emulated ionic NIC exactly as rocm-ernic has always "
    "presented\n"
    "# it.  It is the base every other profile layers on, so it sets every "
    "key.\n"
    "\n"
    "[identity]\n"
    "# none, capri, elba, giglio or salina (IONIC_ASIC_TYPE_*)\n"
    "asic_type = none\n"
    "asic_rev = 0\n"
    "# 1 to 31 printable characters; ethtool -i shows it as firmware-version.\n"
    "fw_version = rocm-ernic-1.0\n"
    "\n"
    "[link]\n"
    "# Mbps: 1000, 10000, 25000, 40000, 50000, 100000, 200000 or 400000\n"
    "speed_mbps = 100000\n"
    "# copper, fiber or none; ethtool shows Direct Attach Copper, FIBRE or "
    "None.\n"
    "xcvr = copper\n"
    "# The largest MTU the guest may set, 1500 to 9582.\n"
    "mtu_max = 9198\n"
    "\n"
    "[queues]\n"
    "# Ethernet RX and TX queues, 1 to 64.  The guest uses min(eth, vCPUs).\n"
    "eth = 4\n"
    "# RDMA event queues, 4 to 32.\n"
    "rdma_eqs = 32\n"
    "# MSI-X vectors, 2 to 128, or auto for 1 + eth + rdma_eqs.  With fewer, "
    "the\n"
    "# driver halves its EQs or queues to fit on a guest with many vCPUs.\n"
    "intr_vectors = 32\n"
    "\n"
    "[stats]\n"
    "# How often the statistics blocks in guest memory are rewritten, 10 to "
    "60000\n"
    "# ms.  The driver never asks, so this is how stale `ip -s link` can be.\n"
    "interval_ms = 1000\n";

/* profiles/small-vm.ini */
static const char small_vm_ini[] =
    "# small-vm: a NIC sized for CI runners and 1-2 vCPU guests: two queue\n"
    "# pairs, the fewest RDMA EQs the driver accepts, and just enough "
    "vectors.\n"
    "\n"
    "[queues]\n"
    "eth = 2\n"
    "rdma_eqs = 4\n"
    "intr_vectors = auto\n";

/* profiles/generic-100g.ini */
static const char generic_100g_ini[] =
    "# generic-100g: a typical modern 100G RoCE NIC.  Not calibrated against "
    "any\n"
    "# real product.  16 queue pairs fit common test VM sizes.\n"
    "\n"
    "[link]\n"
    "speed_mbps = 100000\n"
    "xcvr = fiber\n"
    "\n"
    "[queues]\n"
    "eth = 16\n"
    "intr_vectors = auto\n";

/* profiles/generic-400g.ini */
static const char generic_400g_ini[] =
    "# generic-400g: a large 400G NIC.  Not calibrated against any real\n"
    "# product.  64 queue pairs only take effect on a guest with 64 vCPUs.\n"
    "\n"
    "[link]\n"
    "speed_mbps = 400000\n"
    "xcvr = fiber\n"
    "\n"
    "[queues]\n"
    "eth = 64\n"
    "intr_vectors = auto\n";

static const struct {
    const char *name;
    const char *text;
} builtins[] = {
    {"compat", compat_ini},
    {"small-vm", small_vm_ini},
    {"generic-100g", generic_100g_ini},
    {"generic-400g", generic_400g_ini},
};

const char *ionic_profile_builtin(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        if (strcmp(builtins[i].name, name) == 0)
            return builtins[i].text;
    return NULL;
}

const char *ionic_profile_builtin_names(void)
{
    return "compat, small-vm, generic-100g, generic-400g";
}
