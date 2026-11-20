#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# M3 (TSO off), M4 (TSO on), M5 (MTU 1500 / 9000). Usage: m3-m5.sh
# shellcheck source=tests/devmem/scripts/stack1/devmem-tx/lib.sh
. "$(dirname "$(readlink -f "$0")")/lib.sh"
BULK="-S 16m -l 1048576 -n 128 -a 65536"                 # 128 MiB, page-aligned
ODD="-S 16m -o 123 -l 100003 -a 7919 -n 600"             # 57 MiB, nothing aligned
setfeat() { # tso gso mtu
    local n
    for n in 1 2; do
        "$S/gssh" $n "sudo ip link set $IF mtu $3; sudo ethtool -K $IF tx on sg on >/dev/null 2>&1; sudo ethtool -K $IF tso $1 gso $2"
    done
    G1 "ping -c 2 -q -M do -s $(( $3 - 28 )) $IP2 | tail -n 2"
}
run() { # label tso gso mtu
    setfeat "$2" "$3" "$4"
    devmem_case "$1-bulk" 5410 "$BULK"
    devmem_case "$1-odd" 5411 "$ODD"
}
case "${1:-all}" in
m3) run M3-tso-off-mtu1500 off on 1500; run M3-tso-gso-off-mtu1500 off off 1500 ;;
m4) run M4-tso-on-mtu1500 on on 1500 ;;
m5) run M5-tso-on-mtu9000 on on 9000; run M5-tso-off-mtu9000 off on 9000; run M5-tso-on-mtu9198 on on 9198 ;;
esac
setfeat on on 1500 >/dev/null
