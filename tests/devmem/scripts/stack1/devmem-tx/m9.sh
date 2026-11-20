#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# M9: teardown and reconfiguration with sends in flight.
# shellcheck source=tests/devmem/scripts/stack1/devmem-tx/lib.sh
. "$(dirname "$(readlink -f "$0")")/lib.sh"
BULK="-S 16m -l 1048576 -a 65536 -n 64"
STREAM="-S 16m -o 123 -l 100003 -a 7919 -n 0"
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
gone() { G1 't0=$(date +%s); for i in $(seq 120); do n=$(sudo awk "/^Total/{print \$2}" /sys/kernel/debug/dma_buf/bufinfo); [ "$n" = 0 ] && break; sleep 1; done; echo "dma-buf objects on sender: $n after $(( $(date +%s) - t0 ))s"'; }
xact() {
    local x
    for x in close-sock unbind free-dmabuf exit; do
        RXWAIT=40 devmem_case "M9-X-$x-with-sends-in-flight" 5470 "$BULK" "-w 1000 -X $x" "-W 20"
        gone
    done
}
linkdown() {
    RXWAIT=60 HOOK_DELAY=3 HOOK="G1 'sudo ip link set $IF down; sleep 2; sudo ip link set $IF up; sleep 1; ip -br addr show $IF'" \
        devmem_case M9-sender-link-down-up-mid-stream 5471 "$STREAM" "-T 15 -u 5000 -R 20 -C 30" "-W 30"
    gone
}
reconfig() {
    RXWAIT=90 HOOK_DELAY=3 HOOK="G1 'for c in \"ethtool -G $IF tx 512\" \"ethtool -G $IF rx 2048\" \"ethtool -G $IF tx 8192\" \"ethtool -L $IF combined 2\" \"ethtool -L $IF combined 1\" \"ethtool -L $IF combined 4\" \"ip link set $IF mtu 1400\" \"ip link set $IF mtu 1500\" \"ethtool -G $IF tx 1024 rx 1024\"; do echo \"\$c -> \$(sudo \$c 2>&1) rc=\$?\"; sleep 1.5; done'" \
        devmem_case M9-ring-channel-mtu-reconfig-mid-stream 5472 "$STREAM" "-T 25 -u 5000 -R 20 -C 30" "-W 30"
    gone
}
"$1"
