#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Module unload under KASAN: (a) orphan connection holding unacked devmem skbs, binding owner gone; (b) live binding.
# shellcheck source=tests/devmem/scripts/tx-verify/v.sh
. "$(dirname "$(readlink -f "$0")")/v.sh"
KO=$(g1 'find ~/ionic-mod/dmtx74-1 -name ionic.ko | head -1')
splat() { "$G" "$1" "sudo dmesg | grep -E 'KASAN|BUG:|Oops|WARNING|DMA-API:|circular locking|refcount_t|sleeping function|use-after-free' | cut -c1-220"; }
case "$1" in
a)
    feat_reset >/dev/null; wait_dmabuf0 90
    g1 "sudo dmesg -C; sudo sysctl -w net.ipv4.tcp_retries2=5"; g2 "sudo dmesg -C"
    HOOK="g1 'sudo pkill -KILL -x vtx; echo killed; sleep 1; ss -tnoi \"dport = :6701\" | cut -c1-200; sudo tail -n 1 /sys/kernel/debug/dma_buf/bufinfo; sudo rmmod ionic; echo rmmod exit \$?; sleep 1; lsmod | grep -c ^ionic; sudo insmod $KO; echo insmod exit \$?; sleep 4; ip -br link show rocm-ernic0'" HOOK_DELAY=3 RXWAIT=40 NOPROBE=1 \
        vcase G-a-rmmod-with-orphan-holding-unacked-devmem-skbs 6701 "--rcvbuf 65536 --stall-after 5000000 --stall-secs 20" "-K -S 16m -T 20 -b 8m" d:123:1000003:0:0:65521
    echo "--- splat-class lines guest 1 (whole log since dmesg -C):"; splat 1
    echo "--- splat-class lines guest 2:"; splat 2
    g1 'sudo systemctl restart rocm-ernic-netcfg.service 2>&1 | tail -1; sleep 2'; feat_reset; wait_dmabuf0 400
    vcase G-a-positive-after-reload 6702 "" "-S 4m -R 32" d:1:1m:0:0:4099
    g1 "sudo sysctl -w net.ipv4.tcp_retries2=15"
    ;;
b)
    feat_reset >/dev/null; wait_dmabuf0 90
    g1 "sudo dmesg -C"
    HOOK="g1 'sudo rmmod ionic; echo rmmod exit \$?; lsmod | grep -c ^ionic'" HOOK_DELAY=4 RXWAIT=30 NOPROBE=1 \
        vcase G-b-rmmod-with-live-binding 6703 "--idle 8" "-S 16m -T 12 -C 2" d:123:1000003:0:0:65521 p:5
    echo "--- guest 1 kernel log since dmesg -C (first 120 lines):"; g1 "sudo dmesg | cut -c1-200 | head -120"
    ;;
esac
echo "DONE g $1"
