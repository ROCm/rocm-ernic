#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Build git-am mbox files in patches/ernic-v7.2.6 from the stage copies in
# ../stages (b0 = v7.2.6 + srq + p4, bN = after patch N), the messages of
# the mainline series (and of v7.2.15's e247d033dd32 for patch 1) and the
# backport notes in ../notes.  Nothing is committed anywhere.
set -euo pipefail
R=${DEVMEM_ROOT:?set it to the work area}
B=$R/backport-scratch
OUT=$R/patches/ernic-v7.2.6
M=$B/msgs
N=7
when=$(date +%s)
mkdir -p "$OUT" "$M" "$B/old"
for f in "$OUT"/0*.patch; do
	[ -e "$f" ] && mv "$f" "$B/old/$(basename "$f").$when"
done
cd "$B/stages"

# format-patch's layout of the diffstat (see patches/scripts/mkpatches.sh)
stat() {
	git apply --stat | perl -e '@l = <STDIN>; $w = 1;
		for (@l) { $w = length($1) if /\|\s+(\d+) / && length($1) > $w }
		for (@l) { s/\|\s+(\d+) /sprintf("| %*d ", $w, $1)/e; print }'
}
slug() { tr '[:upper:]' '[:lower:]' | sed -e "s/'//g" -e 's/[^a-z0-9_]\+/-/g' -e 's/^-//' -e 's/-$//'; }
mkdiff() { # prev cur
	{ git diff --no-index --no-color --no-renames --abbrev=12 "b$1" "b$2" || true; } |
		sed -E -e "s#^diff --git a/b$1/(.*) b/b$2/(.*)\$#diff --git a/\1 b/\2#" \
		       -e "s#^--- a/b$1/#--- a/#" -e "s#^\+\+\+ b/b$2/#+++ b/#"
}

# Messages.  1: the stable commit, as v7.2.15 has it.  2..7: mainline 1..6.
git -C "$R/linux" log -1 --format=%s e247d033dd32 >"$M/1.subj"
git -C "$R/linux" log -1 --format=%b e247d033dd32 | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}' >"$M/1.body"
echo "Nikhil P. Rao <nikhil.rao@amd.com>" >"$M/1.from"
echo "Tue, 1 Sep 2026 05:56:27 +0000" >"$M/1.date"
n=1
for p in "$R"/patches/mainline/000[1-6]-*.patch; do
	n=$((n + 1))
	git mailinfo "$M/$n.body" /dev/null <"$p" >"$M/$n.info"
	sed -n 's/^Subject: //p' "$M/$n.info" >"$M/$n.subj"
	echo "$(sed -n 's/^Author: //p' "$M/$n.info") <$(sed -n 's/^Email: //p' "$M/$n.info")>" >"$M/$n.from"
	date -R -d @$((when + n)) >"$M/$n.date"
done

for n in $(seq 1 $N); do
	subj=$(cat "$M/$n.subj")
	out=$OUT/000$n-$(echo "$subj" | slug).patch
	d=$(mkdiff $((n - 1)) "$n")
	{
		echo "From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001"
		echo "From: $(cat "$M/$n.from")"
		echo "Date: $(cat "$M/$n.date")"
		echo "Subject: [PATCH] $subj"
		echo
		# body without trailing blank lines, then the note
		sed -e :a -e '/^\n*$/{$d;N;ba' -e '}' "$M/$n.body"
		echo
		cat "$B/notes/$n.txt"
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
