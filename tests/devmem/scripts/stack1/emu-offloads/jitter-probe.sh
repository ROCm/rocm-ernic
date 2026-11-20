#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# jitter-probe.sh OUTFILE: latency-tail probe, guest 1 -> guest 2, with
# whatever server/driver is running. Stop-and-wait UDP echo RTTs and a
# stop-and-wait MSG_ZEROCOPY TCP sender (64 KiB chunks, each waiting for its
# completion notification), counting tail-loss-probe retransmits.
set -euo pipefail
S=${DEVMEM_ROOT:?set it to the work area}/test/scripts
G=/mnt/hostfs/test/scripts/emu-offloads/gt.py
for n in 1 2; do "$S/gssh" $n "bash /mnt/hostfs/test/scripts/emu-offloads/rx-start.sh"; done
{
    echo "== $(date '+%F %T') server: $(cat "${DEVMEM_ROOT:?set it to the work area}/test/stack/server-binary.path" 2>/dev/null || echo image)"
    "$S/gssh" 1 "uname -r; ethtool -k rocm-ernic0 | grep -E '^(tx-checksumming|scatter-gather|tcp-segmentation-offload):'
    nstat -n
    echo '-- udp stop-and-wait, 3 x 30000 datagrams of 376 bytes'
    for i in 1 2 3; do python3 $G udp 192.168.100.12 5400 376 --repeat 30000 | python3 -c 'import sys,json; d=json.loads(sys.stdin.read()); print(d[\"sent\"], d[\"good\"], d[\"lost\"], d[\"rtt_ms\"])'; done
    echo '-- tcp zerocopy stop-and-wait, 100 x 4000000 bytes (62 chunks each)'
    for i in \$(seq 100); do python3 $G tx 192.168.100.12 5400 4000000 --mode zerocopy; done | python3 -c '
import sys,json
r=[json.loads(l) for l in sys.stdin]
print(len(r), \"runs, all match:\", all(x[\"match\"] for x in r), \"retrans total:\", sum(x[\"total_retrans\"] for x in r), \"copied sends:\", sum(x[\"zc_copied_sends\"] for x in r), \"median MiB/s:\", sorted(x[\"MiB_s\"] for x in r)[50])'
    nstat | grep -E 'TcpRetransSegs|TCPLossProbes|TCPDSACKRecv |TCPTimeouts|TCPLostRetransmit|TCPFastRetrans' || true"
} 2>&1 | tee "$1"
