#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# splat.sh [GUESTS]  print kernel-log lines that indicate a bug, per guest; exit 1 if any.
# One known, driver-independent report is counted but not shown: DMA-API "mapping sg segment
# longer than device claims to support", raised when the core maps the udmabuf at bind
# (ionic, like bnxt/fbnic/gve, sets no dma max segment size).
S=$(dirname "$(readlink -f "$0")")
rc=0
for n in ${1:-1 2}; do
    out=$("$S/gssh" "$n" 'sudo dmesg' | awk '
        /cut here/ { blk = $0 "\n"; inblk = 1; next }
        inblk { blk = blk $0 "\n"; if ($0 ~ /end trace/) { if (blk ~ /mapping sg segment longer/) known++; else printf "%s", blk; inblk = 0; blk = "" } next }
        { print }
        END { if (inblk) printf "%s", blk; if (known) print "KNOWN-SG-SEGMENT-WARNINGS " known > "/dev/stderr" }' 2>/tmp/splat-known.$$ |
        grep -n -E "BUG:|WARNING:|KASAN|[Ll]ockdep|possible (circular|recursive)|inconsistent lock|suspicious RCU|hung task|blocked for more|csum failure|page_pool_release_retry|stalled pool|Call Trace|refcount_t|DMA-API|use-after-free|sleeping function|RTNL: assertion|held lock" |
        grep -v -E "amdgpu|Lockdep tracker|lockdep_|taints kernel|RCU lockdep checking|DMA-API: (preallocated|debugging enabled)|Automounting of tracing")
    known=$(cat /tmp/splat-known.$$ 2>/dev/null); rm -f /tmp/splat-known.$$
    if [ -n "$out" ]; then echo "rx-$n SPLAT:"; echo "$out" | head -n 40; rc=1; else echo "rx-$n: clean ${known:+($known)}"; fi
done
exit $rc
