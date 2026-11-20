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
bound() { "$S/devmem-rx.sh" start 1 | tail -n 1; pools 1; }
finish() { send_fg "${1:-4194304}"; "$S/devmem-rx.sh" wait 1 300; wait_no_dmabuf_pool 1; echo "   counters: $("$S/counters.sh" 1)"; "$S/splat.sh" 1; }

hdr "T2: bind, then ifdown/ifup with the binding in place, then send (binding must survive)"
bound; g 1 "sudo ip link set $IF down; sudo ip link set $IF up; sudo ip addr replace $IP1/24 dev $IF; for i in \$(seq 50); do ping -c1 -W1 $IP2 >/dev/null 2>&1 && break; sleep 0.2; done; echo ping tries \$i"; pools 1; finish

hdr "T3: bind, then operations the core or driver must refuse or survive, then send"
bound
g 1 "sudo ethtool -G $IF tcp-data-split off; echo \"tcp-data-split off with binding: rc=\$? (expect refusal)\"
sudo ip link set dev $IF xdp pinned /sys/fs/bpf/xdp_pass; echo \"xdp attach with binding: rc=\$? (expect refusal)\"
sudo ethtool -G $IF rx 1024; echo \"ring 1024 with binding: rc=\$?\"; sudo ethtool -G $IF rx 4096; echo \"ring 4096 with binding: rc=\$?\"
sudo ip link set $IF mtu 9000; echo \"mtu 9000 with binding: rc=\$?\"; sudo ip link set $IF mtu 1500; echo \"mtu 1500 with binding: rc=\$?\"
sudo ethtool -L $IF combined 2; echo \"channels 2 with binding on q0: rc=\$?\"; sudo ethtool -L $IF combined 1; echo \"channels 1: rc=\$?\"
ethtool -g $IF | grep -i -E 'split|^RX:'; for i in \$(seq 50); do ping -c1 -W1 $IP2 >/dev/null 2>&1 && break; sleep 0.2; done; echo ping tries \$i" 2>&1
pools 1; finish

hdr "T4: bind while the netdev is down (driver says -ENETDOWN)"
g 1 "sudo ip link set $IF down; sudo timeout 20 ncdevmem -n -l -s $IP1 -p 5201 -f $IF -t 0 -q 1 -v 7 -L 2>&1 | tail -n 3; echo rc=\$?; sudo ip link set $IF up; sudo ip addr replace $IP1/24 dev $IF"
"$S/splat.sh" 1

hdr "T5: ifdown mid-stream, wait, ifup; stream should resume or the sender time out; then kill"
bound; stream_bg 400000000; wait_rx_bytes 20000000
g 1 "sudo ip link set $IF down; echo down; sudo ip link set $IF up; sudo ip addr replace $IP1/24 dev $IF; echo up"
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
g 2 'for i in $(seq 450); do [ -e /tmp/sender.rc ] && break; sleep 0.2; done; cat /tmp/sender.rc 2>/dev/null || echo "sender still running after 90 s"'
"$S/devmem-rx.sh" wait 1 100; g 1 'sudo pkill -x ncdevmem'; g 2 'pkill socat'; wait_no_dmabuf_pool 1; echo "   counters: $("$S/counters.sh" 1)"

hdr "T6: ethtool -K rx off with a binding: devmem payload must be dropped (unreadable_drop), not delivered; rx on again recovers"
bound; g 1 "sudo ethtool -K $IF rx off"; echo "   counters: $("$S/counters.sh" 1)"
send_fg 100000 "" 8; echo "   counters after send with rx off: $("$S/counters.sh" 1)"; "$S/devmem-rx.sh" summary 1 | head -n 2
g 1 "sudo ethtool -K $IF rx on; sudo pkill -x ncdevmem"; wait_no_dmabuf_pool 1
hdr "health after T2-T6"; health
hdr "first light again"; first_light
