#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Reduced re-run for the other kernel (7.2.15-devmem, ERNIC configuration).
# shellcheck source=tests/devmem/scripts/tx-verify/v.sh
. "$(dirname "$(readlink -f "$0")")/v.sh"
feat_reset; wait_dmabuf0 90
vcase F-A1-bind-tso-on 6501 "" "-S 4m -R 64" d:1:1m:0:0:4099
PRE1="sudo ethtool -K $IF tso off" vcase F-A2-tso-off-unaligned 6502 "" "-S 4m -R 64" d:13:999983:0:0:4099
feat_reset >/dev/null
vcase F-B1-480x128B-gaps-tso-on 6503 "" "-S 4m -R 600" d:7:61440:128:61:977
vcase F-B3b-exactly-15-frags 6504 "" "-S 4m -R 300 -N" d:5:960:64:64:4096 p:15
vcase F-D2-interleave-devmem-copy-userzc 6505 "" "-S 4m -R 400" d:1:70001:0:0:4099 c:5:1:0:0:13 d:9:3:0:0:4099 c:77:30011:0:0:4099 z:4095:50021:0:0:4099 d:4094:5:0:0:4096 c:1:1449:0:0:7 d:11:200003:4096:1:977
g1 'sudo sysctl -w net.ipv4.tcp_retries2=5'
RXWAIT=12 vcase F-B6-16-frags-one-over 6506 "--idle 8" "-S 4m -R 1 -N -t 4 -C 2" d:5:1024:64:64
wait_dmabuf0 90
RXWAIT=12 vcase F-B8-1024x16B-tiny-iovecs 6507 "--idle 8" "-S 4m -R 1 -t 4 -C 2" d:9:16384:16:16
wait_dmabuf0 90
port=6510
for combo in "tx off" "sg off" "tx off sg off"; do
    tag=${combo// /-}
    feat_reset >/dev/null; wait_dmabuf0 90; port=$((port + 1))
    PRE1="sudo ethtool -K $IF $combo; ethtool -k $IF | grep -E '^(tx-checksumming|scatter-gather|tcp-segmentation-offload|generic-segmentation-offload):' | tr -s ' \n' ' '" \
        RXWAIT=14 vcase "F-C-before-connect-$tag" $port "--idle 9" "-S 4m -R 20 -t 5 -C 2" d:123:100003:0:0:7919
    feat_reset >/dev/null; wait_dmabuf0 90; port=$((port + 1))
    HOOK="g1 'sudo ethtool -K $IF $combo; ethtool -k $IF | grep -E \"^(tx-checksumming|scatter-gather|tcp-segmentation-offload|generic-segmentation-offload):\" | tr -s \" \n\" \" \"'" HOOK_DELAY=3 \
        RXWAIT=20 vcase "F-C-mid-stream-$tag" $port "--idle 9" "-S 16m -T 9 -t 5 -C 2" d:123:100003:0:0:7919 p:10
done
feat_reset >/dev/null; wait_dmabuf0 90
for n in 1 2; do "$G" $n "sudo ip link set $IF mtu 9000"; done
RXWAIT=14 vcase F-D10c-mtu9000-120x512B-iovecs 6520 "--idle 9" "-S 4m -R 100 -t 5 -C 2" d:7:61440:512:61:977
feat_reset >/dev/null; wait_dmabuf0 90
g1 'sudo sysctl -w net.ipv4.tcp_retries2=15'
HOOK="g1 'sudo pkill -KILL -x vtx; echo killed'" HOOK_DELAY=3 RXWAIT=30 vcase F-D8-sigkill-sender-mid-stream 6521 "" "-K -S 16m -T 20 -b 8m" d:123:1000003:0:0:65521
wait_dmabuf0 60
RXWAIT=90 vcase F-D4-zero-window-stall-resume 6522 "--rcvbuf 65536 --stall-after 20000000 --stall-secs 12 --stalls 2" "-S 16m -R 60" d:123:1000003:0:0:65521
vcase F-positive-last 6523 "" "-S 4m -R 32" d:1:1m:0:0:4099
echo "DONE f"
