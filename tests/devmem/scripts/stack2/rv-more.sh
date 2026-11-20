#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Review round: devmem RX cases that were NOT RUN before: split-interrupt mode, XDP refusal
# with a binding (pinned program present), IPv6, VLAN.
# shellcheck source=tests/devmem/scripts/stack2/tdlib.sh
. "$(dirname "$(readlink -f "$0")")/tdlib.sh"
IF=rocm-ernic0
"$S/td-setup.sh" 1 | tail -n 3
hdr "split interrupts (rx 1 tx 1), bind, 16 MiB, ring resize with binding, 16 MiB, unbind"
g 1 "sudo ethtool -L $IF rx 1 tx 1 combined 0; ethtool -l $IF | tail -n 4 | tr -s '\t\n' ' '; echo; for i in \$(seq 50); do ping -c1 -W1 $IP2 >/dev/null 2>&1 && break; sleep 0.2; done"
first_light 16777216; wait_no_dmabuf_pool 1
"$S/devmem-rx.sh" start 1 | tail -n 1; g 1 "sudo ethtool -G $IF rx 2048; echo ring rc=\$?; sudo ethtool -G $IF rx 1024"; pools 1
send_fg 16777216; "$S/devmem-rx.sh" wait 1 100; wait_no_dmabuf_pool 1
g 1 "sudo ethtool -L $IF combined 1 rx 0 tx 0; ethtool -l $IF | tail -n 1"
hdr "XDP attach with a binding (core must refuse)"
g 1 "sudo test -e /sys/fs/bpf/xdp_pass || sudo python3 /mnt/hostfs/test-rx/guest/xdp_pin.py /sys/fs/bpf/xdp_pass"
"$S/devmem-rx.sh" start 1 | tail -n 1
g 1 "sudo ip link set dev $IF xdp pinned /sys/fs/bpf/xdp_pass; echo \"xdp attach with binding: rc=\$?\""
g 1 'sudo pkill -x ncdevmem'; wait_no_dmabuf_pool 1
hdr "devmem RX over IPv6 (fd00:100::/64), 16 MiB"
g 1 "sudo ip -6 addr replace fd00:100::11/64 dev $IF nodad"; g 2 "sudo ip -6 addr replace fd00:100::12/64 dev $IF nodad; for i in \$(seq 30); do ping -6 -c1 -W1 fd00:100::11 >/dev/null 2>&1 && break; sleep 0.2; done; echo ping6 tries \$i"
"$S/devmem-rx.sh" start 1 "-n -l -s fd00:100::11 -p 5201 -f $IF -t 0 -q 1 -v 7 -L" | tail -n 1
g 2 "$PAT | head -c 16777216 | timeout 60 socat -u - 'TCP6:[fd00:100::11]:5201'; echo sender rc=\$?"; "$S/devmem-rx.sh" wait 1 100
echo "   counters: $("$S/counters.sh" 1)"; wait_no_dmabuf_pool 1
hdr "devmem RX over VLAN 100 (192.168.110.0/24), 16 MiB"
for n in 1 2; do g $n "sudo ip link add link $IF name v100 type vlan id 100 2>/dev/null; sudo ip addr replace 192.168.110.1$n/24 dev v100; sudo ip link set v100 up"; done
g 2 "for i in \$(seq 30); do ping -c1 -W1 192.168.110.11 >/dev/null 2>&1 && break; sleep 0.2; done; echo vlan ping tries \$i"
"$S/devmem-rx.sh" start 1 "-n -l -s 192.168.110.11 -p 5201 -f $IF -t 0 -q 1 -v 7 -L" | tail -n 1
g 2 "$PAT | head -c 16777216 | timeout 60 socat -u - TCP:192.168.110.11:5201; echo sender rc=\$?"; "$S/devmem-rx.sh" wait 1 100
echo "   counters: $("$S/counters.sh" 1)"; wait_no_dmabuf_pool 1
for n in 1 2; do g $n "sudo ip link del v100; sudo ip -6 addr del fd00:100::1$n/64 dev $IF"; done
health
