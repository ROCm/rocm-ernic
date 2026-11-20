#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Copy each stage's files into the tree in order and build ionic.
# usage: build-series.sh BUILD_DIR TAG "STAGES" [extra make args...]
# Leaves the tree at the last stage built; run with s5 last.
set -uo pipefail
T=${DEVMEM_ROOT:?set it to the work area}/tools
export PATH=$T/bison/usr/bin:$PATH BISON_PKGDATADIR=$T/bison/usr/share/bison
L=${DEVMEM_ROOT:?set it to the work area}/linux
S=${DEVMEM_ROOT:?set it to the work area}/patches/stages
D=drivers/net/ethernet/pensando/ionic
B=$1; TAG=$2; STAGES=$3; shift 3
cd "$L" || exit 1
for n in $STAGES; do
	cp -r "$S/s$n/." "$L/"
	log="$B/series-$TAG-s$n.log"
	make O="$B" -j64 "$@" $D/ >"$log" 2>&1
	rc=$?
	echo "$TAG s$n: rc=$rc CC=$(grep -c '^  CC \[M\]' "$log") CHECK=$(grep -c '^  CHECK .*ionic' "$log") warnings=$(grep -ci 'warning' "$log") errors=$(grep -ci 'error' "$log") ionic.o=$(stat -c %s "$B/$D/ionic.o" 2>/dev/null)"
	grep -i 'warning\|error' "$log" | head -20
done
