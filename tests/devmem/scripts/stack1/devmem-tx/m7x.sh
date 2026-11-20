#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# M7 extras. nobackstop: the series without patch 0006 loaded on the sender,
# SG and checksum off. orphan: where a frag page outside the test's dmabuf comes from.
# shellcheck source=tests/devmem/scripts/stack1/devmem-tx/lib.sh
. "$(dirname "$(readlink -f "$0")")/lib.sh"
restore() { G1 "sudo ethtool -K $IF tx on sg on; sudo ethtool -K $IF tso on gso on"; }
case "$1" in
nobackstop)
    G1 "sudo ethtool -K $IF tx off sg off"
    CAP=1 RXWAIT=30 devmem_case "M7-NOBACKSTOP-tx-sg-off-before-connect-100KB-sends" 5455 "-S 4m -o 123 -l 100003 -a 7919 -n 20" "-t 6 -C 3" "-W 12"
    restore ;;
orphan)
    G1 "sudo ethtool -K $IF tx off"
    RXWAIT=30 devmem_case "M7-orphan-1-tx-off-1KB-send-stalls" 5456 "-S 4m -o 123 -l 1000 -a 7919 -n 5" "-N -w 0 -t 6 -C 3" "-W 12"
    G1 "sudo cp /tmp/dtx-pfns /tmp/dtx-pfns.orphan; ss -tin state all dst $IP2 | cut -c1-200"
    restore
    RXWAIT=60 devmem_case "M7-orphan-2-next-stream-with-tx-on" 5457 "-S 16m -o 123 -l 100003 -a 7919 -n 0" "-T 40 -u 20000 -C 10" "-W 12"
    echo "--- frag pages of other connections vs the dmabuf of case orphan-1 (port 5456)"
    G1 "sudo cat /tmp/dtx-pfns.orphan" | while read -r h; do echo $((16#$h)); done | sort -n >"$SNAPDIR/orphan.pfns"
    awk '$1 != 5457 {print $2}' "$SNAPDIR/probe.portpfns" | sort -n >"$SNAPDIR/other.pfns"
    echo "other-connection frag pages: $(wc -l <"$SNAPDIR/other.pfns") (ports: $(awk '$1 != 5457 {print $1}' "$SNAPDIR/probe.portpfns" | sort -u | tr '\n' ' ')); of these in the orphan-1 dmabuf: $(comm -12 "$SNAPDIR/other.pfns" "$SNAPDIR/orphan.pfns" | wc -l)"
    ;;
esac
