#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# bench.sh [SAMPLES]  -- rate samples for five transfer kinds, 64 MiB per run
# in 1 MiB sends, no probe.
#
# These are emulation numbers on a loaded host and vary by more than 2x
# between samples: they bound the order of magnitude and nothing more.  The
# VRAM row is dominated by the two CPU mappings of emulated VRAM that the
# sender fills and the receiver verifies through, not by the datapath.
set -uo pipefail
HERE=$(dirname "$(readlink -f "$0")")
# shellcheck source=tests/devmem/scripts/stack3/env.sh
. "$HERE/env.sh"

R=${1:-4}
P=5600

run() { # tag txsrc rxsrc
    for i in $(seq "$R"); do
        P=$((P + 1))
        PROBE=0 "$HERE/case.sh" "BM-$1-$i" "$P" "$2" "$3" 64m 64 >/dev/null 2>&1
        awk -v t="$1" -v i="$i" '
          /^RESULT tx /{for(j=1;j<=NF;j++){split($j,a,"=");if(a[1]=="MiB/s")tx=a[2];if(a[1]=="total_secs")ts=a[2]}}
          /^RESULT rx /{for(j=1;j<=NF;j++){split($j,a,"=");if(a[1]=="MiB/s")rx=a[2]}}
          /^RESULT (tx|rx) FAIL/{bad=1}
          END{printf "%-22s sample %s: tx %6s MiB/s (%ss)  rx %6s MiB/s%s\n", t, i, tx, ts, rx, (bad?"  FAIL":"")}' \
            "$EV/BM-$1-$i.txt"
    done
}

run copy-tcp copy plain
run msg-zerocopy zc plain
run devmemtx-plainrx udmabuf plain
run devmem-udma-udma udmabuf udmabuf
run vram-to-vram gem-vram gem-vram
