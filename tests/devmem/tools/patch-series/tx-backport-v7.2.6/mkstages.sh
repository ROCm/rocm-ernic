#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Build stages/b0..b7 under backport-scratch.  b0 = the files the series
# touches, from base/ (v7.2.6 + srq + p4).  b1 = b0 + the v7.2.15 stable
# commit e247d033dd32.  b(N+1) = bN + mainline patch N of the devmem series,
# without its Documentation/ hunk (not in the sparse checkout).
set -euo pipefail
R=${DEVMEM_ROOT:?set it to the work area}
B=$R/backport-scratch
S=$B/stages
ETH=drivers/net/ethernet/pensando/ionic
FILES="ionic_dev.h ionic_lif.c ionic_lif.h ionic_stats.c ionic_txrx.c"
[ -e "$S" ] && { echo "$S exists" >&2; exit 1; }
mkdir -p "$S/b0/$ETH"
for f in $FILES; do cp "$B/base/$ETH/$f" "$S/b0/$ETH/"; done

next() { # n: copy b(n-1) to bn and cd there
	cp -r "$S/b$(($1 - 1))" "$S/b$1"
	cd "$S/b$1"
}

next 1
git -C "$R/linux" show --format= e247d033dd32 -- "$ETH" >"$S/stable-e247d033dd32.diff"
git apply --verbose "$S/stable-e247d033dd32.diff"

n=1
for p in "$R"/patches/mainline/000[1-6]-*.patch; do
	n=$((n + 1))
	next $n
	echo "== b$n: $(basename "$p")"
	git apply --verbose --exclude='Documentation/*' "$p"
done
echo "== ionic_txrx.c: b1 vs v7.2.15, b7 vs the patched v7.4-rc3 tree"
cmp "$S/b1/$ETH/ionic_txrx.c" "$R/linux-7.2.15/$ETH/ionic_txrx.c" && echo "b1 txrx identical to v7.2.15"
for f in ionic_txrx.c ionic_dev.h ionic_stats.c; do
	cmp "$S/b7/$ETH/$f" "$R/linux/$ETH/$f" && echo "b7 $f identical to patched v7.4-rc3"
done
