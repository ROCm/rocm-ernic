#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Runs INSIDE the devmem-kbuild container. Stack 2's kernel flavour:
# test/kbuild/7.4-rc3 (copied, with its .config) + VLAN_8021Q=y + INFINIBAND_IONIC=m.
set -euo pipefail
T=${DEVMEM_ROOT:?set it to the work area}/test-rx
SRC=$T/kbuild/7.4-rc3-vlan/src
OUT=$T/kbuild/7.4-rc3-vlan/out
export HOME=/tmp KBUILD_BUILD_USER=devmem KBUILD_BUILD_HOST=testbed
cd "$SRC"
# the copied tree has absolute paths of the original in generated Makefiles
"$SRC/scripts/config" --file "$OUT/.config" -e VLAN_8021Q -m INFINIBAND_IONIC
make O="$OUT" olddefconfig
grep -E 'CONFIG_(VLAN_8021Q|INFINIBAND_IONIC|KASAN|PROVE_LOCKING)[ =]' "$OUT/.config"
make O="$OUT" -j"${JOBS:-56}" LOCALVERSION=-devmem-kasan-vlan \
    DEB_BUILD_PROFILES='pkg.linux-upstream.nokerneldbg' bindeb-pkg
echo "=== BUILD DONE ==="
ls -la "$T/kbuild/7.4-rc3-vlan"/*.deb
