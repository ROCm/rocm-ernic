#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Copy each stage's files into the tree in order and build ionic with W=1.
# usage: build-stages.sh BUILD_DIR TAG [extra make args...]
set -uo pipefail
T=${DEVMEM_ROOT:?set it to the work area}/tools
export PATH=$T/bison/usr/bin:$PATH BISON_PKGDATADIR=$T/bison/usr/share/bison
L=${DEVMEM_ROOT:?set it to the work area}/linux
S=${DEVMEM_ROOT:?set it to the work area}/patches/stages
B=$1; TAG=$2; shift 2
cd "$L" || exit 1
for n in 0 1 2 3 4 5; do
	cp -r "$S/s$n/." "$L/"
	log="$B/build-$TAG-s$n.log"
	make O="$B" -j64 W=1 "$@" drivers/net/ethernet/pensando/ionic/ >"$log" 2>&1
	rc=$?
	echo "stage s$n: rc=$rc CC=$(grep -c '^  CC' "$log") warnings=$(grep -ci 'warning' "$log") errors=$(grep -c 'error' "$log")"
done
