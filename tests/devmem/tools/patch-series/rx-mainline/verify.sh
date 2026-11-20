#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Verify the series: stage s0 is v7.4-rc3 plus the XDP frag offset fix for
# net (patches/xdp-frag-offset) plus the six netmem Tx patches,
# the Rx patches reproduce the stages from it in order, git mailinfo parses
# them, checkpatch --strict, and the linux-rx tree equals the whole series.
set -uo pipefail
R=${DEVMEM_ROOT:?set it to the work area}
P=$R/patches-rx
L=$R/linux-rx
V=$P/verify
ts=$(date +%s)
mkdir -p "$P/old"
[ -d "$V" ] && mv "$V" "$P/old/verify.$ts"
mkdir -p "$V/tree" "$V/mailinfo"
FILES=$(cd "$P/stages/s0" && find . -type f | sed 's#^\./##' | sort)
echo "== base: v7.4-rc3 (aebb72744764) blobs + XDP frag offset fix + netmem Tx series vs stage s0"
for f in $FILES; do
	mkdir -p "$V/tree/$(dirname "$f")"
	git -C "$L" show "aebb72744764:$f" >"$V/tree/$f"
done
cd "$V/tree" || exit 1
for p in "$R"/patches/xdp-frag-offset/net/0001-*.patch; do
	git apply --whitespace=error-all "$p"; echo "   net $(basename "$p"): git apply rc=$?"
done
for p in "$R"/patches/mainline/000[1-9]-*.patch; do
	git apply --whitespace=error-all "$p"; echo "   tx $(basename "$p"): git apply rc=$?"
done
bad=0
for f in $FILES; do cmp -s "$f" "$P/stages/s0/$f" || { bad=1; echo "   MISMATCH vs s0: $f"; }; done
[ $bad = 0 ] && echo "   identical to stage s0 ($(echo "$FILES" | wc -l) files)"
n=0
for p in "$P"/mainline/000[1-9]-*.patch; do
	n=$((n + 1))
	echo "== $(basename "$p")"
	git apply --check --verbose "$p" 2>&1 | sed 's/^/   check: /'
	echo "   git apply --check rc=${PIPESTATUS[0]}"
	git apply --whitespace=error-all "$p"; echo "   git apply rc=$?"
	bad=0
	for f in $FILES; do cmp -s "$f" "$P/stages/s$n/$f" || { bad=1; echo "   MISMATCH vs s$n: $f"; }; done
	[ $bad = 0 ] && echo "   result identical to stage s$n"
done
echo "== linux-rx working tree vs result of the whole series"
bad=0
for f in $FILES; do cmp -s "$f" "$L/$f" || { bad=1; echo "   MISMATCH: $f"; }; done
[ $bad = 0 ] && echo "   identical ($n patches)"
echo "   git status --short in linux-rx:"; git -C "$L" status --short | sed 's/^/     /'
echo "   HEAD: $(git -C "$L" log --oneline -1)"
echo "== XDP frag loop: every stage uses the buffer's own page_offset"
for n in 0 1 2 3 4 5 6 7 8; do
	f=$P/stages/s$n/drivers/net/ethernet/pensando/ionic/ionic_txrx.c
	echo "   s$n: bi->page_offset uses=$(grep -c 'bi->page_offset' "$f") buf_info->page_offset in frag loop=$(sed -n '/sinfo->nr_frags++/,/remain_len -= frag_len/p' "$f" | grep -c 'buf_info->page_offset') frag filled at 0=$(grep -c 'skb_frag_fill.*desc(frag, bi->[a-z]*, 0,' "$f")"
done
echo "== git mailinfo"
for p in "$P"/mainline/*.patch; do
	b=$(basename "$p" .patch)
	git mailinfo "$V/mailinfo/$b.msg" "$V/mailinfo/$b.diff" <"$p" >"$V/mailinfo/$b.info"
	echo "-- $b rc=$? msg_lines=$(wc -l <"$V/mailinfo/$b.msg") diff_lines=$(wc -l <"$V/mailinfo/$b.diff")"
	sed 's/^/   /' "$V/mailinfo/$b.info"
done
echo "== checkpatch --strict $*"
cd "$L" || exit 1
for p in "$P"/mainline/*.patch; do
	echo "-- $(basename "$p")"
	# linux-rx is a partial clone: checkpatch asks git about every hash
	# in a message, and for one the clone does not have (the cover
	# letter's prerequisite-patch-id lines) git goes to the remote, for
	# minutes each.  Without a GIT_DIR checkpatch does not ask.
	case "$p" in
	*/0000-*) gd=/nonexistent ;;
	*) gd=.git ;;
	esac
	GIT_DIR=$gd ./scripts/checkpatch.pl --strict "$@" "$p" 2>&1 | grep -v '^$' | sed 's/^/   /'
done
