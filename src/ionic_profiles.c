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

#include "ionic_profile.h"

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
    "[stats]\n"
    "# How often the statistics blocks in guest memory are rewritten, 10 to "
    "60000\n"
    "# ms.  The driver never asks, so this is how stale `ip -s link` can be.\n"
    "interval_ms = 1000\n";

/* profiles/small-vm.ini */
static const char small_vm_ini[] =
    "# small-vm: a NIC sized for CI runners and 1-2 vCPU guests.\n"
    "#\n"
    "# Its queue and interrupt keys arrive with configurable queues; until "
    "then\n"
    "# it is the same device as compat.\n";

static const struct {
    const char *name;
    const char *text;
} builtins[] = {
    {"compat", compat_ini},
    {"small-vm", small_vm_ini},
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
    return "compat, small-vm";
}
