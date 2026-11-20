#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# M8 with own tools: devmem send on a socket routed out of a netdev other than the bound one.
# shellcheck source=tests/devmem/scripts/tx-verify/v.sh
. "$(dirname "$(readlink -f "$0")")/v.sh"
feat_reset >/dev/null; wait_dmabuf0 90
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
pcapsum='sudo tcpdump -r /tmp/enp.pcap -nn 2>/dev/null | awk "{ if (\$0 ~ /length [1-9][0-9]*\$/) d++; else o++ } END { print \"enp0s5 capture: frames with TCP payload:\", d+0, \"without:\", o+0 }"'
echo "===== CASE E1-destination-routed-via-enp0s5  $(date -u +%FT%TZ)"
g1 "ip route get 10.0.2.2; sudo rm -f /tmp/enp.pcap; sudo nohup tcpdump -i enp0s5 -s 0 -w /tmp/enp.pcap tcp port 2222 >/dev/null 2>&1 & sleep 2
    b=\$(cat /sys/class/net/enp0s5/statistics/tx_dropped); u=\$(sudo ethtool -S rocm-ernic0 | awk '/ tx_unreadable_drop/{print \$2}')
    sudo vtx -B -S 4m -R 20 -t 5 -C 2 rocm-ernic0 10.0.2.2 2222 d:123:100003:0:0:7919; echo \"sender exit status: \$?\"
    sleep 1; sudo pkill -INT -x tcpdump; sleep 1; $pcapsum
    echo \"enp0s5 tx_dropped \$b -> \$(cat /sys/class/net/enp0s5/statistics/tx_dropped); tx_unreadable_drop \$u -> \$(sudo ethtool -S rocm-ernic0 | awk '/ tx_unreadable_drop/{print \$2}'); dmabufs \$(sudo awk '/^Total/{print \$2}' /sys/kernel/debug/dma_buf/bufinfo)\"
    sudo dmesg | tail -n 3 | cut -c1-160"
g1 "sudo rm -f /tmp/enp.pcap; sudo nohup tcpdump -i enp0s5 -s 0 -w /tmp/enp.pcap tcp port 6402 >/dev/null 2>&1 & sleep 2; cat /sys/class/net/enp0s5/statistics/tx_dropped"
HOOK="g1 'sudo ip route add $IP2/32 via 10.0.2.2 dev enp0s5; ip route get $IP2'" HOOK_DELAY=3 RXWAIT=20 \
    vcase E2-route-moved-to-enp0s5-mid-stream 6402 "--idle 9" "-B -S 16m -T 9 -t 5 -C 2" d:123:100003:0:0:7919 p:10
g1 "sudo ip route del $IP2/32 via 10.0.2.2 dev enp0s5; ip route get $IP2; sudo pkill -INT -x tcpdump; sleep 1; $pcapsum; sudo tcpdump -r /tmp/enp.pcap -nn 2>/dev/null | head -12; echo enp0s5 tx_dropped now \$(cat /sys/class/net/enp0s5/statistics/tx_dropped)"
wait_dmabuf0 120
vcase E3-positive-after 6403 "" "-S 4m -R 32" d:1:1m:0:0:4099
echo "DONE wrongdev"
