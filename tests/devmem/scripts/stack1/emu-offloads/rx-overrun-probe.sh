#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# rx-overrun-probe.sh OUTFILE [RING...]: is loss under bulk TCP an Rx-ring
# overrun at the receiver? For each Rx ring size on the receiver (devmem-1),
# send 6 x 1 GiB devmem-2 -> devmem-1 over IPv6 (hash-checked) and print per
# transfer: sender retransmits (TCP_INFO) and the receiver's hw_rx_dropped
# and emulator rx_queue_empty deltas.
set -uo pipefail
S=${DEVMEM_ROOT:?set it to the work area}/test/scripts
G=/mnt/hostfs/test/scripts/emu-offloads/gt.py
out=$1; shift
rings=${*:-256 1024 4096}
emu() { docker cp ionic-devmem-ernic-1-1:/run/ernic-stats/ernic-1.stats - | tar -xO | awk '/rx_queue_empty/ {print $3}'; }
{
    echo "== $(date '+%F %T') server: $(cat "${DEVMEM_ROOT:?set it to the work area}/test/stack/server-binary.path" 2>/dev/null || echo image); $("$S/gssh" 1 uname -r)"
    for r in $rings; do
        "$S/gssh" 1 "sudo ethtool -G rocm-ernic0 rx $r; for i in \$(seq 100); do ping -q -c1 -W1 fd00:100::12 >/dev/null 2>&1 && break; sleep 0.2; done; bash /mnt/hostfs/test/scripts/emu-offloads/rx-start.sh; ethtool -g rocm-ernic0 | sed -n '/Current/,\$p' | grep '^RX:' | tr -s '\t' ' '"
        for i in 1 2 3 4 5 6; do
            d0=$("$S/gssh" 1 "ethtool -S rocm-ernic0 | awk '/hw_rx_dropped/ {print \$2}'")
            line=$("$S/gssh" 2 "python3 $G tx fd00:100::11 5400 1073741824" | python3 -c 'import sys,json; d=json.loads(sys.stdin.read()); print("match", d["match"], "MiB/s", d["MiB_s"], "retrans", d["total_retrans"], "dsack", d["dsack_dups"])')
            sleep 2.2   # the emulator rewrites its stats file every second
            d1=$("$S/gssh" 1 "ethtool -S rocm-ernic0 | awk '/hw_rx_dropped/ {print \$2}'")
            echo "  rx ring $r, 1 GiB #$i: $line; receiver hw_rx_dropped +$((d1 - d0)); emulator rx_queue_empty total $(emu)"
        done
    done
    "$S/gssh" 1 "sudo ethtool -G rocm-ernic0 rx 1024; ethtool -g rocm-ernic0 | sed -n '/Current/,\$p' | grep '^RX:' | tr -s '\t' ' '"
} 2>&1 | tee "$out"
