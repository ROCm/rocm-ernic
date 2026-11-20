#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# M9, module unload. a: unbound, but unacked devmem data still queued (receiver stopped).
# b: live binding and a running stream (suspected core use-after-free; run last).
# shellcheck source=tests/devmem/scripts/stack1/devmem-tx/lib.sh
. "$(dirname "$(readlink -f "$0")")/lib.sh"
MOD=${MOD:-ernic-dmtx1}
unload() { G1 '[ -e /sys/module/ionic_rdma ] && sudo rmmod ionic_rdma; sudo rmmod ionic; echo "rmmod rc=$?"; lsmod | grep -c "^ionic"'; }
reload() { G1 "cd ~/ionic-mod/$MOD && sudo insmod ./ionic.ko && { [ ! -e ./ionic_rdma.ko ] || sudo insmod ./ionic_rdma.ko; }; for i in \$(seq 100); do [ -e /sys/class/net/$IF ] && break; sleep 0.2; done; sudo systemctl restart rocm-ernic-netcfg.service; sleep 2; ${ALT:+sudo ip addr add 10.213.100.11/24 dev $IF;} ip -br addr show $IF; ping -c 2 -q $IP2 | tail -n 2"; }
rxcap_start() { G2 "sudo rm -f /tmp/dtx-rx.pcap; sudo nohup tcpdump -i $IF -U -s 100 -w /tmp/dtx-rx.pcap tcp port $1 >/dev/null 2>&1 & sleep 1"; }
rxcap_stop() { # NAME : fetch the receiver guest's capture, list its RST frames and the last frames
    G2 "sudo pkill -INT -x tcpdump; sleep 0.5; sudo cat /tmp/dtx-rx.pcap" >"$E/pcap/$1.receiver-guest.pcap"
    echo "--- receiver-guest capture: frames $(tcpdump -nn -r "$E/pcap/$1.receiver-guest.pcap" 2>/dev/null | wc -l); RST frames:"
    tcpdump -nn -e -tttt -r "$E/pcap/$1.receiver-guest.pcap" 'tcp[tcpflags] & tcp-rst != 0' 2>/dev/null | head -n 5
    echo "--- last 6 frames:"; tcpdump -nn -tttt -r "$E/pcap/$1.receiver-guest.pcap" 2>/dev/null | tail -n 6 | cut -c1-200
}
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
state() { G1 'echo "dma-buf objects: $(sudo awk "/^Total/{print \$2}" /sys/kernel/debug/dma_buf/bufinfo); dma-api error_count $(sudo cat /sys/kernel/debug/dma-api/error_count); sockets to peer: $(ss -tn state all dst '"$IP2"' | tail -n +2 | wc -l)"'; }
[ -n "${ALT:-}" ] && altaddr
case "$1" in
a)
    echo "===== CASE M9-rmmod-after-unbind-with-unacked-data  $(date -u +%FT%TZ)"
    mark M9-rmmod-a; snap before
    start_rx 5480 "-S 16m -l 1048576 -a 65536 -n 64" "-W 60"
    rxcap_start 5480
    G2 "pkill -STOP -x devmem_tx; echo receiver stopped"
    start_probe
    G1 "sudo $DT -f $IF -s $IP2 -p 5480 -S 16m -l 1048576 -a 65536 -n 64 -D -F /tmp/dtx-pfns -w 1000 -t 5 -C 3 -X unbind" 2>&1 | tee "$SNAPDIR/tx.out"
    echo "sender exit status: ${PIPESTATUS[0]}"
    G1 "ss -tinem state all dst $IP2 | cut -c1-220"; state
    echo "--- rmmod with the orphaned connection holding unacked devmem skbs"
    unload; sleep 2; state
    echo "--- bpftrace probe"; stop_probe
    echo "--- insmod again, resume the receiver"
    reload
    G2 "pkill -CONT -x devmem_tx"; wait_rx 70
    rxcap_stop M9-rmmod-a
    # shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
    G1 't0=$(date +%s); for i in $(seq '"${GONE_WAIT:-200}"'); do n=$(sudo awk "/^Total/{print \$2}" /sys/kernel/debug/dma_buf/bufinfo); [ "$n" = 0 ] && break; sleep 1; done; echo "dma-buf objects on sender: $n after $(( $(date +%s) - t0 ))s"'
    state; sleep 1.2; snap after; snapdiff before after
    DMESG_MAX=120 dmesg_since
    ;;
b)
    echo "===== CASE M9-rmmod-with-live-binding-and-running-stream  $(date -u +%FT%TZ)"
    mark M9-rmmod-b; snap before
    start_rx 5481 "-S 16m -o 123 -l 100003 -a 7919 -n 0" "-W 20"
    rxcap_start 5481
    G1 "sudo $DT -f $IF -s $IP2 -p 5481 -S 16m -o 123 -l 100003 -a 7919 -n 0 -D -T 20 -u 5000 -t 8 -C 5" >"$SNAPDIR/tx.out" 2>&1 &
    txpid=$!
    sleep 4; state
    echo "--- rmmod while the sender holds its binding and is sending"
    unload; state
    wait $txpid; echo "sender exit status: $?"; cat "$SNAPDIR/tx.out"
    echo "--- the sender has exited: its netlink socket (the binding) is closed now"
    sleep 2; state; G1 'uptime; sudo dmesg | tail -n 3 | cut -c1-200'
    wait_rx 30
    echo "--- insmod again"
    reload
    rxcap_stop M9-rmmod-b
    # shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
    G1 't0=$(date +%s); for i in $(seq 200); do n=$(sudo awk "/^Total/{print \$2}" /sys/kernel/debug/dma_buf/bufinfo); [ "$n" = 0 ] && break; sleep 1; done; echo "dma-buf objects on sender: $n after $(( $(date +%s) - t0 ))s"'
    state; sleep 1.2; snap after; snapdiff before after
    DMESG_MAX=200 dmesg_since
    ;;
esac
