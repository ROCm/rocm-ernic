#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# hds-misc.sh: HDS-on cases beyond bulk TCP. Both guests must have tcp-data-split on.
S=$(dirname "$(readlink -f "$0")")
IP1=192.168.100.11 IP2=192.168.100.12
L1=fe80::706f:63ff:fe6d:1%rocm-ernic0 L2=fe80::706f:63ff:fe6d:2%rocm-ernic0
NT=/mnt/hostfs/test-rx/guest/nettool.py
g() { "$S/gssh" "$@"; }
trace() { # guest seconds
    g "$1" "sudo pkill bpftrace; nohup sudo bpftrace /mnt/hostfs/test-rx/guest/skbshape.bt $2 > /tmp/skbshape.txt 2>&1 &
for i in \$(seq 150); do grep -q Attach /tmp/skbshape.txt 2>/dev/null && break; sleep 0.2; done"
}
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
shape() { g "$1" 'for i in $(seq 200); do pgrep bpftrace >/dev/null || break; sleep 0.2; done; grep -E "headlen|linear_only|^\[|ip_summed|unreadable" /tmp/skbshape.txt | grep -v tcp_v4'; }
cnt() { echo "   counters rx-$1: $("$S/counters.sh" "$1")"; }
hdr() { echo; echo "=== $*"; }

hdr "state"; for n in 1 2; do g $n 'ethtool -g rocm-ernic0 | grep -i split; ip -br link show rocm-ernic0; sysctl -n net.ipv4.tcp_timestamps'; cnt $n; done

hdr "small packets (1..5 byte segments), TCP timestamps ON: expect split (headlen 52), 2->1"
trace 1 12; "$S/xfer.sh" 2 1 $IP1 $IP1 6000 small; shape 1; cnt 1

hdr "small packets, TCP timestamps OFF on sender: frames padded to 60, expect NOT split, still correct, 2->1"
g 2 'sudo sysctl -q -w net.ipv4.tcp_timestamps=0'; trace 1 12; "$S/xfer.sh" 2 1 $IP1 $IP1 6000 small; shape 1; cnt 1
g 2 'sudo sysctl -q -w net.ipv4.tcp_timestamps=1'

hdr "ARP + ping (56, 1472, 2000 = IP fragments), both ways"
for n in 1 2; do p=$([ $n = 1 ] && echo $IP2 || echo $IP1)
  g $n "sudo ip neigh flush dev rocm-ernic0; for s in 56 1472 2000; do ping -c 3 -i 0.2 -W 2 -s \$s -I rocm-ernic0 $p | grep -E 'transmitted'; done; ip neigh show dev rocm-ernic0 | head -n 2"; done

hdr "UDP self-checking datagrams (sizes 1,18,100,1472,2000,8000), 1->2 and 2->1"
for src in 1 2; do dst=$((3-src)); da=$([ $dst = 1 ] && echo $IP1 || echo $IP2)
  g $dst "nohup python3 $NT udp-recv $da 5320 8 > /tmp/udp.out 2>&1 & for i in \$(seq 50); do grep -q listening /tmp/udp.out && break; sleep 0.1; done"
  echo "   $src->$dst $(g $src "python3 $NT udp-send $da 5320 200 1,18,100,1472,2000,8000")"
  # shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
  echo "   $(g $dst 'for i in $(seq 120); do [ $(wc -l < /tmp/udp.out) -ge 2 ] && break; sleep 0.1; done; tail -n 1 /tmp/udp.out')"; cnt $dst; done

hdr "IPv6: ping6 + TCP over link-local, both ways (expect split, headlen 72 = 40+32)"
g 1 "ping -6 -c 3 -i 0.2 $L2 | grep transmitted"; g 2 "ping -6 -c 3 -i 0.2 $L1 | grep transmitted"
trace 2 10; "$S/xfer.sh" 1 2 "$L2" "$L2" 16000000 bulk; shape 2; cnt 2
"$S/xfer.sh" 2 1 "$L1" "$L1" 16000000 bulk; cnt 1
hdr "IPv6 UDP 1->2"
g 2 "nohup python3 $NT udp-recv '$L2' 5321 5 > /tmp/udp.out 2>&1 & for i in \$(seq 50); do grep -q listening /tmp/udp.out && break; sleep 0.1; done"
echo "   $(g 1 "python3 $NT udp-send '$L2' 5321 100 1,100,1400,3000")"
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
echo "   $(g 2 'for i in $(seq 90); do [ $(wc -l < /tmp/udp.out) -ge 2 ] && break; sleep 0.1; done; tail -n 1 /tmp/udp.out')"

hdr "MTU 9000 on both (payload spans several SG elements), TCP both ways"
for n in 1 2; do g $n 'sudo ip link set rocm-ernic0 mtu 9000; ethtool -g rocm-ernic0 | grep -i split'; done
g 1 "for i in \$(seq 30); do ping -c1 -W1 $IP2 >/dev/null && break; sleep 0.2; done; ping -c 2 -M do -s 8972 $IP2 | grep transmitted"
trace 2 10; "$S/xfer.sh" 1 2 $IP2 $IP2 64000000 bulk; shape 2; cnt 2
"$S/xfer.sh" 2 1 $IP1 $IP1 64000000 bulk; cnt 1
for n in 1 2; do g $n 'sudo ip link set rocm-ernic0 mtu 1500'; done
g 1 "for i in \$(seq 30); do ping -c1 -W1 $IP2 >/dev/null && break; sleep 0.2; done"

hdr "ethtool -K rx off on receiver 2 (software checksum, readable buffers), TCP 1->2"
g 2 'sudo ethtool -K rocm-ernic0 rx off'; trace 2 8; "$S/xfer.sh" 1 2 $IP2 $IP2 16000000 bulk; shape 2; cnt 2; g 2 'sudo ethtool -K rocm-ernic0 rx on'

hdr "GRO off on receiver 2, TCP 1->2"
g 2 'sudo ethtool -K rocm-ernic0 gro off'; "$S/xfer.sh" 1 2 $IP2 $IP2 16000000 bulk; g 2 'sudo ethtool -K rocm-ernic0 gro on'

hdr "exclusions on guest 1: XDP attach with HDS on must fail; HDS on with XDP attached must fail"
g 1 'sudo ip link set dev rocm-ernic0 xdp pinned /sys/fs/bpf/xdp_pass; echo "xdp attach with HDS on: rc=$?"
sudo ethtool -G rocm-ernic0 tcp-data-split off; echo "split off rc=$?"; sudo ip link set dev rocm-ernic0 xdp pinned /sys/fs/bpf/xdp_pass; echo "xdp attach with HDS off: rc=$?"
sudo ethtool -G rocm-ernic0 tcp-data-split on; echo "split on with xdp attached: rc=$?"; sudo ip link set dev rocm-ernic0 xdp off; echo "xdp off rc=$?"
sudo ethtool -G rocm-ernic0 tcp-data-split on; echo "split on rc=$?"; ethtool -g rocm-ernic0 | grep -i split' 2>&1
"$S/xfer.sh" 2 1 $IP1 $IP1 4000000 bulk

hdr "ifdown/ifup, ring resize, channel change with HDS on (guest 1), then TCP 2->1"
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
g 1 'A=$(ip -4 -o addr show dev rocm-ernic0 | awk "{print \$4}"); sudo ip link set rocm-ernic0 down; sudo ip link set rocm-ernic0 up; sudo ip addr replace $A dev rocm-ernic0
sudo ethtool -G rocm-ernic0 rx 512; echo "ring 512 rc=$?"; sudo ethtool -G rocm-ernic0 rx 4096; echo "ring 4096 rc=$?"; sudo ethtool -L rocm-ernic0 combined 2; echo "chan 2 rc=$?"; sudo ethtool -L rocm-ernic0 combined 4; echo "chan 4 rc=$?"; ethtool -g rocm-ernic0 | grep -i -E "split|^RX:"
for i in $(seq 30); do ping -c1 -W1 192.168.100.12 >/dev/null && break; sleep 0.2; done; echo "ping tries $i"'
trace 1 8; "$S/xfer.sh" 2 1 $IP1 $IP1 16000000 bulk; shape 1; cnt 1

hdr "tcp-data-split off again on both; TCP both ways; expect frag path (napi_gro_frags), csum_complete"
for n in 1 2; do g $n 'sudo ethtool -G rocm-ernic0 tcp-data-split off; ethtool -g rocm-ernic0 | grep -i split'; done
g 1 "for i in \$(seq 30); do ping -c1 -W1 $IP2 >/dev/null && break; sleep 0.2; done"
trace 2 8; "$S/xfer.sh" 1 2 $IP2 $IP2 16000000 bulk; shape 2; cnt 2; "$S/xfer.sh" 2 1 $IP1 $IP1 16000000 bulk; cnt 1
hdr "splat check"; "$S/splat.sh"
