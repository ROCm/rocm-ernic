#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# D6 follow-up: why did a receiver link flap reset the connection? Hypothesis: with rocm-ernic0 down, guest 2's
# ACKs follow the default route out of enp0s5 and SLIRP answers the stray segment with a RST.
# shellcheck source=tests/devmem/scripts/tx-verify/v.sh
. "$(dirname "$(readlink -f "$0")")/v.sh"
feat_reset >/dev/null; wait_dmabuf0 90
g2 "sudo rm -f /tmp/enp2.pcap; sudo nohup tcpdump -i enp0s5 -s 96 -w /tmp/enp2.pcap tcp port 6230 >/dev/null 2>&1 & sleep 2"
HOOK="g2 'sudo ip link set $IF down; sleep 2; sudo ip link set $IF up; ip -br addr show $IF'" HOOK_DELAY=3 RXWAIT=60 \
    vcase D6-rerun1-receiver-link-flap-capture-on-enp0s5 6230 "" "-S 16m -T 10 -C 30" d:123:1000003:0:0:65521
g2 "sudo pkill -INT -x tcpdump; sleep 1; echo 'receiver enp0s5 capture, port 6230:'; sudo tcpdump -r /tmp/enp2.pcap -nn 2>/dev/null | head -8"
feat_reset >/dev/null; wait_dmabuf0 90
g2 "sudo ip route add blackhole 10.213.100.0/24 metric 4000; ip route show | grep 10.213"
HOOK="g2 'sudo ip link set $IF down; sleep 2; sudo ip link set $IF up; ip -br addr show $IF'" HOOK_DELAY=3 RXWAIT=90 \
    vcase D6-rerun2-receiver-link-flap-with-blackhole-fallback-route 6231 "" "-S 16m -T 10 -C 30" d:123:1000003:0:0:65521
g2 "sudo ip route del blackhole 10.213.100.0/24 metric 4000; ip route show | grep 10.213"
echo "DONE d6"
