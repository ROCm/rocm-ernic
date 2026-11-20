#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Diagnose corrupt multi-buffer XDP echoes: capture on both emulators' tap0
# while guest 2 sends 8000-byte datagrams that guest 1's XDP program echoes,
# with a TCP stream guest 1 -> guest 2 in MODE (devmem|zc|copy|none).
#   m-xdp-diag.sh NAME OBJ SEC MODE [UDPSIZE] [MTU]
# shellcheck source=tests/devmem/scripts/stack1/devmem-tx/lib.sh
. "$(dirname "$(readlink -f "$0")")/lib.sh"
name=$1 obj=$2 sec=$3 mode=$4 size=${5:-8000} mtu=${6:-9000}
echo "===== DIAG $name: xdp_$obj, stream mode $mode, UDP size $size, MTU $mtu  $(date -u +%FT%TZ)"
for n in 1 2; do "$S/gssh" $n "sudo ip link set $IF mtu $mtu"; done
G1 "sudo ethtool -L $IF combined 1; [ $obj = none ] || sudo ip link set dev $IF xdpdrv obj /tmp/xdp_$obj.o sec $sec; for i in \$(seq 50); do ping -c 1 -W 1 -q $IP2 >/dev/null 2>&1 && break; sleep 0.2; done; uname -r; cat /sys/module/ionic/taint; ip -d link show $IF | grep -o 'prog/xdp id [0-9]*'"
mark "$name"; snap before
for n in 1 2; do
    docker rm -f "ionic-devmem-cap-$n" >/dev/null 2>&1 || true
    docker create --cgroup-parent=system.slice --name "ionic-devmem-cap-$n" --network "container:ionic-devmem-ernic-$n-1" --cap-drop ALL --cap-add NET_RAW --cap-add NET_ADMIN \
        -v "$T/bin:/cap:ro" --entrypoint /cap/pcapcap qemu-tool/ernic-tapsetup:1 tap0 /tmp/cap.pcap 900 >/dev/null
    docker start "ionic-devmem-cap-$n" >/dev/null
done
sleep 1
if [ "$mode" != none ]; then
    start_rx 5510 "-S 16m -o 123 -l 100003 -a 7919 -n 0" "-W 30"
    G1 "sudo $DT -f $IF -s $IP2 -p 5510 -S 16m -o 123 -l 100003 -a 7919 -n 0 -m $mode -T 14 -u 50000 -C 30" >"$SNAPDIR/tx.out" 2>&1 &
    txpid=$!
    sleep 2
fi
G2 "nstat -az 2>/dev/null | grep -E 'UdpInCsumErrors|UdpInErrors|UdpRcvbufErrors' | tr -s ' \n' ' '; echo; python3 /tmp/udp_echo_check.py $IP1 10 $size 1000; nstat -az 2>/dev/null | grep -E 'UdpInCsumErrors|UdpInErrors|UdpRcvbufErrors' | tr -s ' \n' ' '; echo"
if [ "$mode" != none ]; then wait "$txpid"; grep RESULT "$SNAPDIR/tx.out"; wait_rx 40 | grep RESULT; fi
for n in 1 2; do cap_stop $n "$E/pcap/$name.tap0-emulator$n.pcap"; python3 "$D/udpsum.py" "$E/pcap/$name.tap0-emulator$n.pcap"; done
sleep 1.2; snap after; snapdiff before after | grep -E 'counters|xdp|drop|err|always'
dmesg_since
G1 "sudo ip link set dev $IF xdp off; sudo ethtool -L $IF combined 4"; for n in 1 2; do "$S/gssh" $n "sudo ip link set $IF mtu 1500"; done
