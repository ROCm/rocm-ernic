#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Stage dkms/b1..b7: the DKMS layout of the base with the files of stage bN
# copied over it, to build the series patch by patch.
set -euo pipefail
B=${DEVMEM_ROOT:?set it to the work area}/backport-scratch
ETH=drivers/net/ethernet/pensando/ionic
for n in 1 2 3 4 5 6 7; do
	d=$B/dkms/b$n
	[ -e "$d" ] && { echo "$d exists" >&2; exit 1; }
	mkdir -p "$d/eth" "$d/rdma" "$d/uapi/rdma"
	cp -r "$B/base/$ETH/." "$d/eth/"
	cp -r "$B/base/drivers/infiniband/hw/ionic/." "$d/rdma/"
	cp "$B/base/include/uapi/rdma/ionic-abi.h" "$d/uapi/rdma/"
	cp "$B/dkms/base/Kbuild" "$d/Kbuild"
	cp "$B/stages/b$n/$ETH"/* "$d/eth/"
done
diff -rq "$B/dkms/b7/eth" "$B/tree/$ETH" && echo "dkms/b7/eth == tree after the series"
