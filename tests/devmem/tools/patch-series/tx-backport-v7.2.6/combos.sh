#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Does the series apply on the other trees rocm-ernic's callers build?
# Each combination: v7.2.6's three directories, the named series with
# "git apply", then patches/ernic-v7.2.6 with "git apply --check" + apply.
set -uo pipefail
R=${DEVMEM_ROOT:?set it to the work area}
B=$R/backport-scratch
E=${ERNIC_REPO:-$HOME/Projects/rocm-ernic}/ansible/files/patches
ETH=drivers/net/ethernet/pensando/ionic
RDMA=drivers/infiniband/hw/ionic
ts=$(date +%s)
[ -e "$B/combos" ] && mv "$B/combos" "$B/old/combos.$ts"
combo() { # name dirs...
	local name=$1 d p rc=0
	shift
	d=$B/combos/$name
	mkdir -p "$d/$ETH" "$d/$RDMA" "$d/include/uapi/rdma"
	cp -r "$R/linux-7.2.6/$ETH/." "$d/$ETH/"
	cp -r "$R/linux-7.2.6/$RDMA/." "$d/$RDMA/"
	cp -r "$R/linux-7.2.6/include/uapi/rdma/." "$d/include/uapi/rdma/"
	cd "$d" || exit 1
	for s in "$@"; do
		for p in "$s"/*.patch; do git apply --whitespace=fix "$p" || rc=1; done
	done
	[ $rc = 0 ] || { echo "$name: base series FAILED"; return; }
	for p in "$R"/patches/ernic-v7.2.6/0*.patch; do
		git apply --check "$p" && git apply --whitespace=error-all "$p" || rc=1
	done
	echo "$name: devmem-tx series rc=$rc; eth files identical to srq+p4 result: $(
		for f in ionic_dev.h ionic_lif.c ionic_stats.c ionic_txrx.c; do
			cmp -s "$ETH/$f" "$B/tree/$ETH/$f" || echo -n "NO($f) "
		done; echo checked)"
}
combo plain
combo top "$E"
combo top-p4 "$E" "$E/p4"
combo srq "$E/srq"
combo srq-p4 "$E/srq" "$E/p4"
