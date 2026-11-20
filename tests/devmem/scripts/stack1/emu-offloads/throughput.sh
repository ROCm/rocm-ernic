#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# throughput.sh OUTFILE: emulation throughput with the current server/driver/
# offload settings, both directions: gt.py 64 MiB + 1 GiB hashed transfers,
# iperf3 TCP (plain and -Z sendfile), 10 s each.
set -euo pipefail
S=${DEVMEM_ROOT:?set it to the work area}/test/scripts
G=/mnt/hostfs/test/scripts/emu-offloads/gt.py
for n in 1 2; do "$S/gssh" $n "bash /mnt/hostfs/test/scripts/emu-offloads/rx-start.sh; pgrep -x iperf3 >/dev/null || iperf3 -s -D; for i in \$(seq 50); do ss -ltnH 'sport = :5201' | grep -q . && break; sleep 0.1; done"; done
{
    echo "== $(date '+%F %T') server: $(cat "${DEVMEM_ROOT:?set it to the work area}/test/stack/server-binary.path" 2>/dev/null || echo image)"
    for src in 1 2; do
        dst=192.168.100.1$((3 - src))
        echo "-- devmem-$src -> $dst"
        "$S/gssh" $src "uname -r; ethtool -k rocm-ernic0 | grep -E '^(tx-checksumming|scatter-gather|tcp-segmentation-offload):' | tr '\n' ' '; echo
        python3 $G tx $dst 5400 67108867 1073741824 | python3 -c '
import sys,json
for l in sys.stdin:
    d=json.loads(l); print(\"gt.py plain\", d[\"bytes\"], \"match\", d[\"match\"], \"MiB/s\", d[\"MiB_s\"], \"retrans\", d[\"total_retrans\"])'
        for z in '' -Z; do iperf3 -c $dst -t 10 \$z -J | python3 -c '
import sys,json
d=json.load(sys.stdin)[\"end\"]; print(\"iperf3 '\$z'\", round(d[\"sum_received\"][\"bits_per_second\"]/1e6), \"Mbit/s received, sender retransmits\", d[\"sum_sent\"][\"retransmits\"])'; done"
    done
} 2>&1 | tee "$1"
