#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Things the two matrix runs did not do.
# shellcheck source=tests/devmem/scripts/tx-verify/v.sh
. "$(dirname "$(readlink -f "$0")")/v.sh"
feat_reset >/dev/null; wait_dmabuf0 90
case "$1" in
interleave)
    vcase D2-interleave-devmem-copy-userzc-one-socket 6202 "" "-S 4m -R 400" d:1:70001:0:0:4099 c:5:1:0:0:13 d:9:3:0:0:4099 c:77:30011:0:0:4099 z:4095:50021:0:0:4099 d:4094:5:0:0:4096 c:1:1449:0:0:7 d:11:200003:4096:1:977
    vcase D2b-interleave-nodelay 6203 "" "-S 4m -R 400 -N" d:1:1000:0:0:4099 c:5:10:0:0:13 d:9:300:0:0:4099 z:4095:5000:0:0:4099 c:77:20:0:0:4099 d:4094:5:0:0:4096
    ;;
oob)
    # iovecs that leave the dmabuf: must be refused, and nothing of them sent
    RXWAIT=8 vcase D3a-iovec-crosses-end-of-dmabuf 6204 "--idle 4" "-S 4m -R 1 -O -t 3 -C 2" d:4096:8192 d:4194000:1000
    RXWAIT=8 vcase D3b-iovec-starts-at-end 6205 "--idle 4" "-S 4m -R 1 -O -t 3 -C 2" d:4096:8192 d:4194304:100
    RXWAIT=8 vcase D3c-iovec-far-beyond 6206 "--idle 4" "-S 4m -R 1 -O -t 3 -C 2" d:4096:8192 d:0x7fffffff0000:4096
    RXWAIT=8 vcase D3d-second-iovec-out-of-range 6207 "--idle 4" "-S 4m -R 1 -O -t 3 -C 2" d:4096:8192 d:4190208:8192:4096:0
    vcase D3e-wraparound-many-passes 6208 "" "-S 1m -R 300" d:1048575:1 d:0:1048576 d:524287:524289 d:3:1048573:0:0:4099
    ;;
zwin)
    # receiver stops reading: zero window, persist probes; then resumes. Twice.
    RXWAIT=90 vcase D4-zero-window-stall-resume 6210 "--rcvbuf 65536 --stall-after 20000000 --stall-secs 12 --stalls 2" "-S 16m -R 60" d:123:1000003:0:0:65521
    ;;
loss)
    # receiver Rx ring of 64: overruns, so fast retransmits, SACK, RTOs of devmem skbs
    g2 "sudo ethtool -G $IF rx 64; ethtool -g $IF | grep -A4 Current | grep RX:"; sleep 2; feat_reset >/dev/null
    RXWAIT=120 vcase D5-receiver-ring-64-retransmits 6211 "" "-S 16m -R 150" d:123:1000003:0:0:65521
    RXWAIT=120 vcase D5b-receiver-ring-64-retransmits-small-iovecs 6212 "" "-S 16m -R 300" d:7:61440:128:61:977
    g2 "sudo ethtool -G $IF rx 1024; ethtool -g $IF | grep -A4 Current | grep RX:"; sleep 2; feat_reset >/dev/null
    ;;
rxflap)
    HOOK="g2 'sudo ip link set $IF down; sleep 2; sudo ip link set $IF up; ip -br addr show $IF'" HOOK_DELAY=3 RXWAIT=60 \
        vcase D6-receiver-link-flap-rto-retransmits 6213 "" "-S 16m -T 10 -C 30" d:123:1000003:0:0:65521
    ;;
rxclose)
    RXWAIT=20 vcase D7-receiver-rst-mid-transfer 6214 "--close-after 30000000 --rst" "-S 16m -R 400 -b 8m -C 3" d:123:1000003:0:0:65521
    wait_dmabuf0 30
    RXWAIT=20 vcase D7b-receiver-fin-mid-transfer 6215 "--close-after 30000000" "-S 16m -R 400 -b 8m -C 3" d:123:1000003:0:0:65521
    wait_dmabuf0 30
    ;;
kill)
    HOOK="g1 'sudo pkill -KILL -x vtx; echo killed'" HOOK_DELAY=3 RXWAIT=30 \
        vcase D8-sigkill-sender-mid-stream 6216 "" "-K -S 16m -T 20 -b 8m" d:123:1000003:0:0:65521
    wait_dmabuf0 60
    # kill while the peer is not reading: the orphan holds devmem skbs, binding owner gone
    HOOK="g1 'sudo pkill -KILL -x vtx; echo killed'" HOOK_DELAY=3 RXWAIT=60 \
        vcase D8b-sigkill-sender-while-receiver-stalled 6217 "--rcvbuf 65536 --stall-after 5000000 --stall-secs 15" "-K -S 16m -T 20 -b 8m" d:123:1000003:0:0:65521
    wait_dmabuf0 120
    ;;
tap)
    g1 "sudo rm -f /tmp/sender-tap.pcap; sudo nohup tcpdump -i $IF -s 0 -w /tmp/sender-tap.pcap tcp port 6218 >/tmp/sender-tap.log 2>&1 & sleep 2"
    vcase D9-packet-tap-on-sender-netdev 6218 "" "-S 4m -R 64" d:13:999983:0:0:4099
    g1 "sudo pkill -INT -x tcpdump; sleep 1; cat /tmp/sender-tap.log; ls -l /tmp/sender-tap.pcap; sudo tcpdump -r /tmp/sender-tap.pcap -nn 2>/dev/null | awk '{ if (\$0 ~ /length [1-9]/) d++; else o++ } END { print \"frames with TCP payload in the sender-side capture:\", d+0, \"without:\", o+0 }'; sudo rm -f /tmp/sender-tap.pcap"
    ;;
jumbo)
    for n in 1 2; do "$G" $n "sudo ip link set $IF mtu 9000"; done
    g1 'sudo sysctl -w net.ipv4.tcp_retries2=5'
    vcase D10a-mtu9000-1MiB-sends 6220 "" "-S 4m -R 64" d:13:999983:0:0:4099
    RXWAIT=14 vcase D10b-mtu9000-480x128B-iovecs 6221 "--idle 9" "-S 4m -R 100 -t 5 -C 2" d:7:61440:128:61:977
    wait_dmabuf0 90
    RXWAIT=14 vcase D10c-mtu9000-120x512B-iovecs 6222 "--idle 9" "-S 4m -R 100 -t 5 -C 2" d:7:61440:512:61:977
    wait_dmabuf0 90
    RXWAIT=14 vcase D10d-mtu9000-60x1024B-iovecs 6223 "--idle 9" "-S 4m -R 100 -t 5 -C 2" d:7:61440:1024:61:977
    wait_dmabuf0 90
    RXWAIT=14 vcase D10e-mtu9000-4096B-unaligned-iovecs 6224 "--idle 9" "-S 4m -R 100 -t 5 -C 2" d:7:245760:4096:61:977
    wait_dmabuf0 90
    g1 'sudo sysctl -w net.ipv4.tcp_retries2=15'
    feat_reset >/dev/null
    ;;
esac
echo "DONE $1"
