#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# M7: TX checksum offload and/or SG turned off on the sender. Nothing
# corrupt may reach the receiver.
# shellcheck source=tests/devmem/scripts/stack1/devmem-tx/lib.sh
. "$(dirname "$(readlink -f "$0")")/lib.sh"
BIG="-S 4m -o 123 -l 100003 -a 7919 -n 20"
SMALL="-S 4m -o 123 -l 1000 -a 7919 -n 5"
restore() { G1 "sudo ethtool -K $IF tx on sg on; sudo ethtool -K $IF tso on gso on; ethtool -k $IF | grep -E '^(tx-checksumming|scatter-gather|tcp-segmentation-offload|generic-segmentation-offload):' | tr -s ' \n' ' '"; echo; }
before() { # label, ethtool args : features changed before the connection is made
    restore >/dev/null
    G1 "sudo ethtool -K $IF $2"
    CAP=1 RXWAIT=30 devmem_case "M7-$1-before-connect-100KB-sends" 5450 "$BIG" "-t 6 -C 3" "-W 12"
    CAP=1 RXWAIT=30 devmem_case "M7-$1-before-connect-1KB-sends" 5451 "$SMALL" "-N -w 0 -t 6 -C 3" "-W 12"
}
during() { # label, ethtool args : features changed 3 s into a stream
    restore >/dev/null
    CAP=1 RXWAIT=40 HOOK="G1 'sudo ethtool -K $IF $2; ethtool -k $IF | grep -E \"^(tx-checksumming|scatter-gather|tcp-segmentation-offload|generic-segmentation-offload):\" | tr -s \" \\n\" \" \"'" HOOK_DELAY=3 \
        devmem_case "M7-$1-during-stream" 5452 "-S 16m -o 123 -l 100003 -a 7919 -n 0" "-T 12 -u 20000 -t 6 -C 3" "-W 12"
}
case "${1:-all}" in
tx) before tx-off "tx off"; during tx-off "tx off" ;;
sg) before sg-off "sg off"; during sg-off "sg off" ;;
both) before tx-sg-off "tx off sg off"; during tx-sg-off "tx off sg off" ;;
gso) before tx-sg-gso-off "tx off sg off gso off"; before tx-gso-off "tx off gso off"; before sg-gso-off "sg off gso off" ;;
esac
restore
