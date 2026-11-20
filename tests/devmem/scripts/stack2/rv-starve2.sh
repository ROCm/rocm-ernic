#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Review round: two Rx queues bound to one dma-buf (16000 pages) with rings of 16384: queue 0
# takes every page at the first fill, queue 1 starts with an EMPTY ring. Does queue 1 get
# buffers, and receive, once queue 0's traffic hands pages back to the pool?
# shellcheck source=tests/devmem/scripts/stack2/tdlib.sh
. "$(dirname "$(readlink -f "$0")")/tdlib.sh"
IF=rocm-ernic0
ppools() { g 1 "sudo python3 /mnt/hostfs/linux-rx/tools/net/ynl/pyynl/cli.py --spec /mnt/hostfs/linux-rx/Documentation/netlink/specs/netdev.yaml --dump page-pool-get --output-json | python3 -c 'import json,sys; print(\"   dmabuf pools (id, inflight):\", [(x[\"id\"], x.get(\"inflight\")) for x in json.load(sys.stdin) if \"dmabuf\" in x])'"; }
probe() { # sport: TCP connect rx-2:sport -> rx-1:5201, 3 s
    g 2 "python3 -c 'import socket,sys
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind((\"$IP2\", $1)); s.settimeout(3)
try:
    s.connect((\"$IP1\", 5201)); print(\"   probe sport $1: CONNECTED\")
except Exception as e: print(\"   probe sport $1: FAILED\", e)'"
}
cnt() { g 1 "ethtool -S $IF | grep -E 'rx_[01]_(pkts|alloc_err):' | tr -s ' ' | tr '\n' ' '"; echo; }
hdr "module text $(g 1 'sudo sha256sum /sys/module/ionic/sections/.text | cut -c1-12')"
"$S/td-setup.sh" 2 | tail -n 3
g 1 "sudo ethtool -G $IF rx 16384; ethtool -g $IF | grep -E '^RX:' | tail -n 1"
PA=$("$S/rxq.py" "$IP2" "$IP1" 5201 2 0); PB=$("$S/rxq.py" "$IP2" "$IP1" 5201 2 1); PB2=$("$S/rxq.py" "$IP2" "$IP1" 5201 2 1 $((PB + 1)))
echo "source ports: queue 0: $PA; queue 1: $PB, $PB2"
hdr "bind queues 0 and 1 to one dma-buf"
"$S/devmem-rx.sh" start 1 "-n -l -s $IP1 -p 5201 -f $IF -t 0 -q 2 -v 7 -L" | tail -n 1
ppools; cnt
hdr "queue 1 has no buffers: a SYN that hashes to it"
probe "$PB"
ppools; cnt
hdr "devmem stream on queue 0 (256 MiB, background)"
stream_bg 268435456 "$PA"; wait_rx_bytes 20000000
for _ in $(seq 20); do ppools; sleep 0.5; done | uniq | head -n 8
hdr "queue 1 again, stream still running"
probe "$PB2"
cnt
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
g 2 'for i in $(seq 600); do [ -e /tmp/sender.rc ] && break; sleep 0.2; done; cat /tmp/sender.rc 2>/dev/null || { echo "sender still running; killing"; pkill socat; }'
"$S/devmem-rx.sh" wait 1 300
cnt
hdr "restore"
g 1 "sudo pkill -x ncdevmem; sleep 1"; wait_no_dmabuf_pool 1
g 1 "sudo ethtool -G $IF rx 1024; sudo ethtool -L $IF combined 1"
health
