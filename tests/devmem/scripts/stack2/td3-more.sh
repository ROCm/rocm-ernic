#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# shellcheck source=tests/devmem/scripts/stack2/tdlib.sh
. "$(dirname "$(readlink -f "$0")")/tdlib.sh"
IF=rocm-ernic0
hdr "T8: two queues, bind queue 1 only, steer by source port (rx-interface T3)"
g 1 "sudo ethtool -L $IF combined 2; ethtool -l $IF | tail -n 1; for i in \$(seq 50); do ping -c1 -W1 $IP2 >/dev/null 2>&1 && break; sleep 0.2; done"
P1=$("$S/rxq.py" "$IP2" "$IP1" 5201 2 1); P0=$("$S/rxq.py" "$IP2" "$IP1" 5310 2 0); P0b=$("$S/rxq.py" "$IP2" "$IP1" 5201 2 0)
echo "source ports: devmem flow -> queue 1: $P1; plain flow to 5310 -> queue 0: $P0; wrong-queue flow to 5201 -> queue 0: $P0b"
"$S/devmem-rx.sh" start 1 "-n -l -s $IP1 -p 5201 -f $IF -t 1 -q 1 -v 7 -L" | tail -n 1; pools 1
g 1 "sudo python3 /mnt/hostfs/linux-rx/tools/net/ynl/pyynl/cli.py --spec /mnt/hostfs/linux-rx/Documentation/netlink/specs/netdev.yaml --dump queue-get --json '{\"ifindex\": '\$(cat /sys/class/net/$IF/ifindex)'}' | tr '\n' ' '"; echo
echo "   counters before: $("$S/counters.sh" 1)"
echo "-- plain TCP on queue 0 while queue 1 is bound:"; "$S/xfer.sh" 2 1 "$IP1" "$IP1" 8000000 bulk "$P0"
echo "-- devmem flow on queue 1:"; send_fg 16777216 "$P1"; "$S/devmem-rx.sh" wait 1 300
echo "   counters after: $("$S/counters.sh" 1)"
echo "-- plain TCP on queue 0 after unbind:"; "$S/xfer.sh" 2 1 "$IP1" "$IP1" 8000000 bulk "$P0"
echo "-- negative: flow to the devmem port that hashes to the unbound queue 0 (ncdevmem must reject: not devmem)"
"$S/devmem-rx.sh" start 1 "-n -l -s $IP1 -p 5201 -f $IF -t 1 -q 1 -v 7 -L" | tail -n 1; send_fg 100000 "$P0b" 20; "$S/devmem-rx.sh" wait 1 100
wait_no_dmabuf_pool 1; "$S/splat.sh" 1
g 1 "sudo ethtool -L $IF combined 1; for i in \$(seq 50); do ping -c1 -W1 $IP2 >/dev/null 2>&1 && break; sleep 0.2; done"

echo "   lockdep after T8: $(g 1 "sudo grep debug_locks /proc/lockdep_stats | tr -s \" \"")"
hdr "T9: PCI function reset (FW down/up path) with a live binding, mid-stream"
"$S/devmem-rx.sh" start 1 | tail -n 1; stream_bg 2000000000; wait_rx_bytes 20000000
g 1 "BDF=\$(basename \$(readlink /sys/class/net/$IF/device)); echo 1 | sudo tee /sys/bus/pci/devices/\$BDF/reset >/dev/null; echo \"pci reset rc=\$?\"; sudo ip addr replace $IP1/24 dev $IF; for i in \$(seq 50); do ping -c1 -W1 $IP2 >/dev/null 2>&1 && break; sleep 0.2; done; echo ping tries \$i"
pools 1; "$S/devmem-rx.sh" summary 1 | head -n 2
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
g 2 'for i in $(seq 100); do [ -e /tmp/sender.rc ] && break; sleep 0.2; done; cat /tmp/sender.rc 2>/dev/null || { echo "sender still running; killing"; pkill socat; }'
g 1 'sudo pkill -x ncdevmem'; wait_no_dmabuf_pool 1; "$S/splat.sh" 1
echo "-- bind again after the reset:"; first_light

echo "   lockdep after T9: $(g 1 "sudo grep debug_locks /proc/lockdep_stats | tr -s \" \"")"
hdr "T7: rmmod ionic_rdma + ionic with a live binding, mid-stream"
g 1 'echo 1 | sudo tee /sys/kernel/debug/dma-api/all_errors >/dev/null; echo 100 | sudo tee /sys/kernel/debug/dma-api/num_errors > /dev/null'
"$S/devmem-rx.sh" start 1 | tail -n 1; stream_bg 2000000000; wait_rx_bytes 20000000
g 1 'sudo timeout 60 rmmod ionic_rdma; echo "rmmod ionic_rdma rc=$?"; sudo timeout 90 rmmod ionic; echo "rmmod ionic rc=$?"; lsmod | grep -c ionic'
"$S/devmem-rx.sh" summary 1 | head -n 2
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
g 1 'for i in $(seq 50); do pgrep -x ncdevmem >/dev/null || break; sleep 0.2; done; pgrep -x ncdevmem >/dev/null && { echo "ncdevmem still alive; killing"; sudo pkill -x ncdevmem; }; grep exit= /tmp/ncdevmem.err'
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
g 2 'for i in $(seq 100); do [ -e /tmp/sender.rc ] && break; sleep 0.2; done; cat /tmp/sender.rc 2>/dev/null || { echo "sender still running; killing"; pkill socat; }'
pools 1; g 1 'sudo dmesg | grep -E "pending DMA|ionic removed" | cut -c1-160; echo 0 | sudo tee /sys/kernel/debug/dma-api/all_errors >/dev/null'; "$S/splat.sh" 1
echo "   lockdep after T7: $(g 1 "sudo grep debug_locks /proc/lockdep_stats | tr -s \" \"")"
