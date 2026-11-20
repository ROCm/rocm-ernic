#!/bin/bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# leak1.sh MODDIR|stock : down, channels 2, up, channels 4, rmmod; print DMA-API pending count before/after
IF=rocm-ernic0; M=$1
echo 1 > /sys/kernel/debug/dma-api/all_errors
ld() { if [ "$M" = stock ]; then modprobe ionic; else insmod "$M"/ionic.ko; insmod "$M"/ionic_rdma.ko; fi
    for _ in $(seq 100); do [ -e /sys/class/net/$IF ] && break; sleep 0.1; done; sleep 1; ip link set $IF up; }
ul() { rmmod ionic_rdma 2>/dev/null; rmmod ionic; }
cnt() { dmesg | grep -o 'pending DMA allocations.*count=[0-9]*' | grep -o '[0-9]*$' | head -1; }
ul; dmesg -C; ld; ul; echo "baseline pending=$(cnt)"
dmesg -C; ld; eval "${2:-ip link set $IF down; ethtool -L $IF combined 2; ip link set $IF up; ethtool -L $IF combined 4}"; ul; echo "after sequence pending=$(cnt)"
dmesg | grep -A12 "Mapped at" | head -14
