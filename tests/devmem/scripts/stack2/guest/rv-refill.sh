#!/bin/bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Review round, run as root in guest 1 with the fault-knob module: an Rx ring that gets NO
# buffers at the first fill, then buffers become available. Nothing is transmitted meanwhile.
IF=rocm-ernic0; P=/sys/module/ionic/parameters
MYIP=$(ip -4 -o addr show dev $IF | awk '{print $4}')
Y=/mnt/hostfs/linux-rx/tools/net/ynl
pools() { python3 $Y/pyynl/cli.py --spec /mnt/hostfs/linux-rx/Documentation/netlink/specs/netdev.yaml --dump page-pool-get --output-json | python3 -c 'import json,sys; print("   pools (id, inflight):", [(x["id"], x.get("inflight")) for x in json.load(sys.stdin) if x.get("ifindex")=='"$(cat /sys/class/net/$IF/ifindex)"'])'; }
st() { ethtool -S $IF | grep -E "^ *(rx_0_alloc_err|rx_0_pkts|tx_0_pkts|tx_packets):" | tr -s ' ' | tr '\n' ' '; echo; }
ethtool -L $IF combined 1; ethtool -G $IF tcp-data-split "${1:-on}"; ethtool -g $IF | grep -i split
echo "== allocation fails (knob), ifdown/ifup: the first fill gets nothing"
echo 1 > $P/fi_rx_alloc_fail
ip link set $IF down; ip link set $IF up; ip addr replace "$MYIP" dev $IF
sleep 1; pools; st; sleep 1; st
echo "== buffers available again (knob off); 0.3 s later, nothing sent by this guest in between"
echo 0 > $P/fi_rx_alloc_fail; sleep 0.3; st; pools
dmesg | grep -E "BUG|WARNING|KASAN" | head -n 5
