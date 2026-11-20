#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Runs INSIDE the devmem-kbuild container.  Build the staged DKMS trees
# (dkms/base, dkms/patched) out of tree against the test bed's built
# v7.2.15 kernel, with the make line of the dkms.conf that
# setup-ionic-dkms.sh writes, plus W=1.
set -uo pipefail
B=${DEVMEM_ROOT:?set it to the work area}/backport-scratch
K=${DEVMEM_ROOT:?set it to the work area}/test/kbuild/7.2.15/out
gcc --version | head -1
echo "kernel: $(cat "$K/include/config/kernel.release")"
for v in "$@"; do
	log=$B/build-$v.log
	make -C "$K" M="$B/dkms/$v" W=1 -j32 modules >"$log" 2>&1
	rc=$?
	echo "$v: rc=$rc CC=$(grep -c '^  CC \[M\]' "$log") LD=$(grep -c '^  LD \[M\]' "$log") warnings=$(grep -ci 'warning' "$log") errors=$(grep -ci 'error' "$log")"
	grep -i 'warning\|error' "$log" | head -20
	ls -l "$B/dkms/$v"/*.ko 2>&1
	modinfo "$B/dkms/$v/ionic.ko" 2>/dev/null | grep -E '^(vermagic|name|depends)'
done
