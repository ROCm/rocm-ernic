#!/bin/bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Review round: bind/unbind (ncdevmem start, kill) in a loop, concurrently with ring, channel
# and MTU changes, ifdown/ifup, PCI function resets and page-pool dumps. Run as root in guest 1.
#   stress.sh SECONDS [nopci]
# Individual operations are allowed to fail (they race each other); what counts is no hang,
# no splat and a working netdev afterwards.
IF=rocm-ernic0; DUR=${1:-60}; NOPCI=${2:-}
BDF=$(basename "$(readlink /sys/class/net/$IF/device)")
MYIP=192.168.100.11
Y=/mnt/hostfs/linux-rx/tools/net/ynl
END=$((SECONDS + DUR))
up() { ip link set $IF up; ip addr replace $MYIP/24 dev $IF; }
binder() {
    local n=0 ok=0
    while [ $SECONDS -lt $END ]; do
        timeout 10 ncdevmem -n -l -s $MYIP -p 5201 -f $IF -t 0 -q 1 >/tmp/stress-ncdevmem.out 2>&1 &
        sleep 0.$((RANDOM % 9 + 1))
        grep -q Waiting /tmp/stress-ncdevmem.out && ok=$((ok + 1))
        pkill -x ncdevmem; wait; n=$((n + 1))
    done
    echo "binder: $n attempts, $ok bound"
}
ops() {
    local n=0 fail=0 rc
    while [ $SECONDS -lt $END ]; do
        case $((RANDOM % (NOPCI ? 7 : 8))) in
        0) timeout 60 ethtool -G $IF rx 512 ;;
        1) timeout 60 ethtool -G $IF rx 2048 ;;
        2) timeout 60 ethtool -L $IF combined 2 ;;
        3) timeout 60 ethtool -L $IF combined 1 ;;
        4) timeout 60 ip link set $IF mtu 3000 ;;
        5) timeout 60 ip link set $IF mtu 1500 ;;
        6) timeout 60 ip link set $IF down; up ;;
        7) echo 1 > /sys/bus/pci/devices/"$BDF"/reset; up ;;
        esac >/dev/null 2>&1
        rc=$?; n=$((n + 1)); [ $rc = 0 ] || fail=$((fail + 1)); [ $rc = 124 ] && echo "ops: TIMEOUT (hang?)"
        sleep 0.$((RANDOM % 5))
    done
    echo "ops: $n operations, $fail refused or failed"
}
dumper() {
    local n=0
    while [ $SECONDS -lt $END ]; do
        python3 $Y/pyynl/cli.py --spec /mnt/hostfs/linux-rx/Documentation/netlink/specs/netdev.yaml --dump page-pool-get >/dev/null 2>&1
        python3 $Y/pyynl/cli.py --spec /mnt/hostfs/linux-rx/Documentation/netlink/specs/netdev.yaml --dump queue-get >/dev/null 2>&1
        ethtool -S $IF >/dev/null 2>&1; n=$((n + 1))
    done
    echo "dumper: $n rounds"
}
dmesg -C
ethtool -L $IF combined 1; ethtool -G $IF tcp-data-split on
binder & ops & dumper &
wait
pkill -x ncdevmem
up; ethtool -L $IF combined 1; ethtool -G $IF rx 1024 tcp-data-split on; ip link set $IF mtu 1500
echo "after: $(ethtool -l $IF | tail -n 1 | tr -s '\t ' ' ') $(ethtool -g $IF | grep -E '^RX:|split' | tail -n 2 | tr -s '\t\n ' ' ') $(ip -br link show $IF | tr -s ' ')"
echo "dmesg: $(dmesg | grep -c 'mapping sg segment longer') known DMA-API sg segment warnings; other reports:"
dmesg | grep -E "BUG:|WARNING:|KASAN|possible (circular|recursive)|inconsistent lock|suspicious RCU|hung task|blocked for more|csum failure|page_pool_release_retry|stalled pool|refcount_t|sleeping function|RTNL: assertion|use-after-free" | grep -v "kernel/dma/debug.c" | head -n 10
echo "lockdep: $(grep debug_locks /proc/lockdep_stats | tr -s ' ')"
