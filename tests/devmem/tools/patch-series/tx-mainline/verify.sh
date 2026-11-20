#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Verify the series: patches reproduce the stages from pristine v7.4-rc3
# files, git mailinfo parses them, checkpatch --strict, tree == full series.
set -uo pipefail
R=${DEVMEM_ROOT:?set it to the work area}
P=$R/patches
L=$R/linux
V=$P/verify
FILES=$(cd "$P/stages/s0" && find . -type f | sed 's#^\./##' | sort)
ts=$(date +%s)
[ -d "$V" ] && mv "$V" "$P/.verify-old-$ts"
mkdir -p "$V/tree" "$V/mailinfo"
echo "== pristine: stage s0 vs v7.4-rc3 (aebb72744764) blobs"
for f in $FILES; do
	mkdir -p "$V/tree/$(dirname "$f")"
	git -C "$L" show "aebb72744764:$f" >"$V/tree/$f"
	cmp -s "$V/tree/$f" "$P/stages/s0/$f" && echo "ok   $f" || echo "FAIL $f"
done
cd "$V/tree" || exit 1
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
echo "== linux working tree vs result of the whole series"
bad=0
for f in $FILES; do cmp -s "$f" "$L/$f" || { bad=1; echo "   MISMATCH: $f"; }; done
[ $bad = 0 ] && echo "   identical ($n patches)"
echo "   git status --short in linux:"; git -C "$L" status --short | sed 's/^/     /'
echo "   HEAD: $(git -C "$L" log --oneline -1) branch $(git -C "$L" branch --show-current)"
echo "== git mailinfo"
for p in "$P"/mainline/*.patch; do
	b=$(basename "$p" .patch)
	git mailinfo "$V/mailinfo/$b.msg" "$V/mailinfo/$b.diff" <"$p" >"$V/mailinfo/$b.info"
	echo "-- $b rc=$? msg_lines=$(wc -l <"$V/mailinfo/$b.msg") diff_lines=$(wc -l <"$V/mailinfo/$b.diff")"
	sed 's/^/   /' "$V/mailinfo/$b.info"
done
echo "== checkpatch --strict"
cd "$L" || exit 1
for p in "$P"/mainline/*.patch; do
	echo "-- $(basename "$p")"
	./scripts/checkpatch.pl --strict "$@" "$p" 2>&1 | grep -v '^$' | sed 's/^/   /'
done
