#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Build the ionic directory of the linux-rx working tree as it is.
# usage: quick.sh [extra make args...]   e.g. quick.sh W=1 C=2 CHECK=...
R=${DEVMEM_ROOT:?set it to the work area}
T=$R/tools
export PATH=$T/bison/usr/bin:$PATH BISON_PKGDATADIR=$T/bison/usr/share/bison
cd "$R"/linux-rx || exit 1
make O="$R"/build-74-rx -j64 "$@" drivers/net/ethernet/pensando/ionic/ 2>&1 | grep -v '^  \(CC\|LD\|AR\|CHECK\|DESCEND\|INSTALL\|GEN\|CALL\|UPD\|HOSTCC\|HOSTLD\|SYNC\|MODPOST\) \|^make\[' ; echo "rc=${PIPESTATUS[0]}"
