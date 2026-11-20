#!/bin/bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# leakbisect.sh MODDIR|stock  -- for each candidate sequence: load, run it, rmmod,
# report the DMA-API "pending allocations" count (cumulative per device; a rise = a leak).
IF=rocm-ernic0; M=$1
echo 1 > /sys/kernel/debug/dma-api/all_errors
ld() {
    if [ "$M" = stock ]; then modprobe ionic; else insmod "$M"/ionic.ko; insmod "$M"/ionic_rdma.ko; fi
    for _ in $(seq 100); do [ -e /sys/class/net/$IF ] && break; sleep 0.1; done
    systemctl reset-failed rocm-ernic-netcfg.service; systemctl restart rocm-ernic-netcfg.service; BDF=$(basename "$(readlink /sys/class/net/$IF/device)")
}
ul() { rmmod ionic_rdma 2>/dev/null; rmmod ionic; }
ul
prev=x
t() {
    dmesg -C; ld; eval "$2" >/dev/null 2>&1; ul
    c=$(dmesg | grep -o 'pending DMA allocations.*count=[0-9]*' | grep -o '[0-9]*$' | head -1)
    echo "$1: pending=${c:-0} $([ "$prev" != x ] && [ "${c:-0}" != "$prev" ] && echo '<== LEAK')"; prev=${c:-0}
}
X="ip link set dev $IF xdp pinned /sys/fs/bpf/xdp_pass"
t "nothing" "true"
t "xdp on, channels 2" "$X; ethtool -L $IF combined 2"
t "xdp on, channels 2, mtu 3000, mtu 1500" "$X; ethtool -L $IF combined 2; ip link set $IF mtu 3000; ip link set $IF mtu 1500"
t "xdp on, down, up, xdp off" "$X; ip link set $IF down; ip link set $IF up; ip link set dev $IF xdp off"
t "xdp on, channels 2, down, up, xdp off, channels 4" "$X; ethtool -L $IF combined 2; ip link set $IF down; ip link set $IF up; ip link set dev $IF xdp off; ethtool -L $IF combined 4"
t "down, ring 512, up" "ip link set $IF down; ethtool -G $IF rx 512 tx 512; ip link set $IF up"
t "down, channels 2, up, channels 4" "ip link set $IF down; ethtool -L $IF combined 2; ip link set $IF up; ethtool -L $IF combined 4"
t "channels 1,2,4" "ethtool -L $IF combined 1;ethtool -L $IF combined 2; ethtool -L $IF combined 4"
t "pci reset up, channels 2, 4" "echo 1 > /sys/bus/pci/devices/$BDF/reset; ethtool -L $IF combined 2; ethtool -L $IF combined 4"
t "pci reset up; down; pci reset; up; channels 2" "echo 1 > /sys/bus/pci/devices/$BDF/reset; ip link set $IF down;echo 1 > /sys/bus/pci/devices/$BDF/reset; ip link set $IF up; ethtool -L $IF combined 2"
t "nothing" "true"
ld
