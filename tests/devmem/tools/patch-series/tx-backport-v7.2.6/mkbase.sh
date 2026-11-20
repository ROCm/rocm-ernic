#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Build DIR (default: base) under backport-scratch: the three directories
# fetch-ionic-sources.sh checks out, from the v7.2.6 worktree, with
# rocm-ernic's srq then p4 series applied by "git apply" (no repository, no
# commit).
set -euo pipefail
R=${DEVMEM_ROOT:?set it to the work area}
B=$R/backport-scratch
E=${ERNIC_REPO:-$HOME/Projects/rocm-ernic}/ansible/files/patches
ETH=drivers/net/ethernet/pensando/ionic
RDMA=drivers/infiniband/hw/ionic
D=$B/${1:-base}
[ -e "$D" ] && { echo "$D exists" >&2; exit 1; }
mkdir -p "$D/$ETH" "$D/$RDMA" "$D/include/uapi/rdma"
cp -r "$R/linux-7.2.6/$ETH/." "$D/$ETH/"
cp -r "$R/linux-7.2.6/$RDMA/." "$D/$RDMA/"
cp -r "$R/linux-7.2.6/include/uapi/rdma/." "$D/include/uapi/rdma/"
cd "$D"
for p in "$E"/srq/*.patch "$E"/p4/*.patch; do
	git apply --whitespace=fix "$p"
	echo "applied $(basename "$p")"
done
