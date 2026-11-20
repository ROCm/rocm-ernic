#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Build git-am mbox files in ../mainline from the per-stage file copies in
# ../stages (s0 = v7.4-rc3 plus the XDP frag offset fix for net plus the
# netmem Tx series, sN = tree after Rx patch N) and the messages in ../msgs.
set -euo pipefail
P=${DEVMEM_ROOT:?set it to the work area}/patches-rx
OUT=$P/mainline
N=8
TAG="RFC PATCH"
AUTHOR="Stephen Bates <sbates@raithlin.com>"
BASE=aebb72744764
when=$(date +%s)
mkdir -p "$OUT" "$P/old"
cd "$P/stages"
for f in "$OUT"/0*.patch; do [ -e "$f" ] && mv "$f" "$P/old/$(basename "$f").$when"; done
# git apply --stat pads the count column to 5; format-patch pads it to the
# widest count.  Same content, format-patch's layout.
stat() {
	git apply --stat | perl -e '@l = <STDIN>; $w = 1;
		for (@l) { $w = length($1) if /\|\s+(\d+) / && length($1) > $w }
		for (@l) { s/\|\s+(\d+) /sprintf("| %*d ", $w, $1)/e; print }'
}
slug() { tr '[:upper:]' '[:lower:]' | sed -e "s/'//g" -e 's/[^a-z0-9]\+/-/g' -e 's/^-//' -e 's/-$//'; }
mkdiff() { # prev cur
	{ git diff --no-index --no-color --no-renames --abbrev=12 "s$1" "s$2" || true; } |
		sed -E -e "s#^diff --git a/s$1/(.*) b/s$2/(.*)\$#diff --git a/\1 b/\2#" \
		       -e "s#^--- a/s$1/#--- a/#" -e "s#^\+\+\+ b/s$2/#+++ b/#"
}
for n in $(seq 1 $N); do
	msg=$P/msgs/000$n.txt
	subj=$(head -1 "$msg")
	out=$OUT/000$n-$(echo "$subj" | slug).patch
	d=$(mkdiff $((n - 1)) "$n")
	{
		echo "From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001"
		echo "From: $AUTHOR"
		echo "Date: $(date -R -d @$((when + n)))"
		echo "Subject: [$TAG $n/$N] $subj"
		tail -n +2 "$msg"
		echo "---"
		echo "$d" | stat
		echo
		echo "$d"
		echo "-- "
		echo "2.43.0"
		echo
	} >"$out"
	echo "wrote $out"
done
# cover letter
d=$(mkdiff 0 $N)
{
	echo "From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001"
	echo "From: $AUTHOR"
	echo "Date: $(date -R -d @"$when")"
	echo "Subject: [$TAG 0/$N] $(head -1 "$P/msgs/0000.txt")"
	tail -n +2 "$P/msgs/0000.txt"
	echo
	echo "Stephen Bates ($N):"
	for n in $(seq 1 $N); do echo "  $(head -1 "$P/msgs/000$n.txt")"; done
	echo
	echo "$d" | stat
	echo
	echo
	echo "base-commit: $(git -C "${DEVMEM_ROOT:?set it to the work area}/linux-rx" rev-parse $BASE)"
	# what this series sits on, in order: the XDP frag offset fix for
	# net, then the netmem Tx series
	for p in "${DEVMEM_ROOT:?set it to the work area}"/patches/xdp-frag-offset/net/0001-*.patch \
		 "${DEVMEM_ROOT:?set it to the work area}"/patches/mainline/000[1-9]-*.patch; do
		echo "prerequisite-patch-id: $(git patch-id --stable <"$p" | cut -d' ' -f1)"
	done
	echo "-- "
	echo "2.43.0"
	echo
} >"$OUT/0000-cover-letter.patch"
echo "wrote $OUT/0000-cover-letter.patch"
