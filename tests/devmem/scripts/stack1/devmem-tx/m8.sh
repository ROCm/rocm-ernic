#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# M8: the route leaves through a netdev other than the one the dmabuf is bound to.
# shellcheck source=tests/devmem/scripts/stack1/devmem-tx/lib.sh
. "$(dirname "$(readlink -f "$0")")/lib.sh"
OTHER=enp0s5
ifstat() { G1 "ip -s link show $OTHER | tail -n 4 | tr -s ' \n' ' '"; echo; }
# a) bound on rocm-ernic0, destination reached through the management NIC from the start.
#    10.0.2.2:2222 is QEMU's SLIRP gateway -> the qemu container's hostfwd -> this guest's sshd.
echo "### M8a: sendmsg to a destination routed via $OTHER (no SO_BINDTODEVICE)"
G1 "ip route get 10.0.2.2"
echo "before: $(ifstat)"
G1 "sudo rm -f /tmp/m8.pcap; sudo nohup tcpdump -i $OTHER -U -w /tmp/m8.pcap tcp port 2222 and host 10.0.2.2 >/dev/null 2>&1 & sleep 1"
TXDST=10.0.2.2 RXWAIT=12 devmem_case M8a-ENODEV-destination-via-mgmt-nic 2222 "-S 4m -o 123 -l 100003 -a 7919 -n 20" "-B -t 6 -C 2" "-W 8"
G1 "sudo pkill -INT -x tcpdump; sleep 0.5; sudo cat /tmp/m8.pcap" >"$E/pcap/M8a.sender-$OTHER.pcap"
pcap_summary "$E/pcap/M8a.sender-$OTHER.pcap" 2222
echo "after: $(ifstat)"
# b) the route moves to the other netdev while data is queued and in flight.
echo "### M8b: route to the receiver moved to $OTHER 3 s into a stream"
G1 "sudo rm -f /tmp/m8.pcap; sudo nohup tcpdump -i $OTHER -U -w /tmp/m8.pcap tcp port 5461 >/dev/null 2>&1 & sleep 1"
CAP=1 RXWAIT=40 HOOK_DELAY=3 HOOK="G1 'sudo ip route add $IP2/32 via 10.0.2.2 dev $OTHER; ip route get $IP2'" \
    devmem_case M8b-route-moved-mid-stream 5461 "-S 16m -o 123 -l 100003 -a 7919 -n 0" "-B -T 12 -u 20000 -t 6 -C 3" "-W 15"
G1 "sudo pkill -INT -x tcpdump; sleep 0.5; sudo cat /tmp/m8.pcap" >"$E/pcap/M8b.sender-$OTHER.pcap"
pcap_summary "$E/pcap/M8b.sender-$OTHER.pcap" 5461
G1 "sudo ip route del $IP2/32 via 10.0.2.2 dev $OTHER; ip route get $IP2; ss -tn state all dst $IP2 | tail -n +2"
echo "after: $(ifstat)"
