#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Silent-corruption trap: checksum / SG offload off, before connect and mid-stream.
# shellcheck source=tests/devmem/scripts/tx-verify/v.sh
. "$(dirname "$(readlink -f "$0")")/v.sh"
port=6100
g1 'sudo sysctl -w net.ipv4.tcp_retries2=5'
for combo in "tx off" "sg off" "tx off sg off" "tx off gso off" "sg off gso off" "tx off sg off gso off"; do
    tag=${combo// /-}
    feat_reset >/dev/null; wait_dmabuf0 90
    port=$((port + 1))
    PRE1="sudo ethtool -K $IF $combo; ethtool -k $IF | grep -E '^(tx-checksumming|scatter-gather|tcp-segmentation-offload|generic-segmentation-offload):' | tr -s ' \n' ' '" \
        RXWAIT=14 vcase "C-before-connect-$tag" $port "--idle 9" "-S 4m -R 20 -t 5 -C 2" d:123:100003:0:0:7919
    feat_reset >/dev/null; wait_dmabuf0 90
    port=$((port + 1))
    HOOK="g1 'sudo ethtool -K $IF $combo; ethtool -k $IF | grep -E \"^(tx-checksumming|scatter-gather|tcp-segmentation-offload|generic-segmentation-offload):\" | tr -s \" \n\" \" \"'" HOOK_DELAY=3 \
        RXWAIT=20 vcase "C-mid-stream-$tag" $port "--idle 9" "-S 16m -T 9 -t 5 -C 2" d:123:100003:0:0:7919 p:10
done
feat_reset; wait_dmabuf0 90
vcase C-positive-after 6199 "" "-S 4m -R 32" d:1:1m:0:0:4099
g1 'sudo sysctl -w net.ipv4.tcp_retries2=15'
echo ALL-DONE
