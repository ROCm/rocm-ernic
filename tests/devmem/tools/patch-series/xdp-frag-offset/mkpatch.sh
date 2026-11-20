#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Build the git-am mbox for the stand-alone XDP frag offset fix from
# stages/s0 (pristine v7.4-rc3 file), stages/s1 (fixed) and msg.txt.
# Not part of the devmem TX series; see notes/test-tx-7.4-rc3.md.
set -euo pipefail
X=${DEVMEM_ROOT:?set it to the work area}/patches/xdp-frag-offset
cd "$X/stages"
d=$({ git diff --no-index --no-color --no-renames --abbrev=12 s0 s1 || true; } |
	sed -E -e 's#^diff --git a/s0/(.*) b/s1/(.*)$#diff --git a/\1 b/\2#' \
	       -e 's#^--- a/s0/#--- a/#' -e 's#^\+\+\+ b/s1/#+++ b/#')
out=$X/net/0001-ionic-use-the-rx-buffers-page-offset-for-xdp-frags.patch
{
	echo "From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001"
	echo "From: Stephen Bates <sbates@raithlin.com>"
	echo "Date: $(date -R)"
	echo "Subject: [PATCH net] $(head -1 "$X/msg.txt")"
	tail -n +2 "$X/msg.txt"
	echo "---"
	echo "$d" | git apply --stat | sed -E 's/\|\s+([0-9]+) /| \1 /'
	echo
	echo "$d"
	echo "-- "
	echo "2.43.0"
	echo
} >"$out"
echo "wrote $out"
