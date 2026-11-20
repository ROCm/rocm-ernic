#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Review round: an Rx queue whose dma-buf (ncdevmem: 16000 pages) cannot cover the ring.
# rv-starve.sh RING [MTU]   (receiver guest 1, combined 1, tcp-data-split on)
# shellcheck source=tests/devmem/scripts/stack2/tdlib.sh
. "$(dirname "$(readlink -f "$0")")/tdlib.sh"
RING=${1:-16384} MTU=${2:-1500}
cnt() { g 1 "ethtool -S rocm-ernic0 | grep -E 'rx_0_(pkts|alloc_err|dropped|unreadable_drop|hds_split):' | tr -s ' ' | tr '\n' ' '"; echo; }
hdr "module: $(g 1 'sha256sum /sys/module/ionic/sections/.text 2>/dev/null | cut -c1-12; ls -l /sys/module/ionic/holders | tail -n +2 | wc -l')  ring $RING mtu $MTU"
"$S/td-setup.sh" 1 | tail -n 3
g 1 "sudo ethtool -G rocm-ernic0 rx 1024; sudo ip link set rocm-ernic0 mtu $MTU"; g 2 "sudo ip link set rocm-ernic0 mtu $MTU"
hdr "bind at ring 1024"
"$S/devmem-rx.sh" start 1 | tail -n 1
g 2 "ping -c 2 -W 1 $IP1 | tail -n 2"
hdr "ethtool -G rx $RING with the binding"
g 1 "sudo ethtool -G rocm-ernic0 rx $RING; echo rc=\$?; ethtool -g rocm-ernic0 | grep -E '^RX:' | tail -n 1"
pools 1
cnt
hdr "ping rx-2 -> rx-1 and rx-1 -> rx-2"
g 2 "ping -c 5 -W 1 $IP1 | tail -n 2"
g 1 "ping -c 5 -W 1 $IP2 | tail -n 2"
cnt
hdr "devmem stream 16 MiB into the short ring"
send_fg 16777216 "" 60
"$S/devmem-rx.sh" wait 1 100
cnt
hdr "unbind (ncdevmem gone), same ring: ordinary pages"
g 1 "sudo pkill -x ncdevmem; sleep 1"
wait_no_dmabuf_pool 1
g 2 "ping -c 3 -W 1 $IP1 | tail -n 2"
"$S/xfer.sh" 2 1 "$IP1" "$IP1" 8000000 bulk
hdr "restore ring 1024, mtu 1500"
g 1 "sudo ethtool -G rocm-ernic0 rx 1024; sudo ip link set rocm-ernic0 mtu 1500"; g 2 "sudo ip link set rocm-ernic0 mtu 1500"
health
