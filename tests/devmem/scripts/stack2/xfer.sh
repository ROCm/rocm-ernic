#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# xfer.sh SRC DST LISTEN_ADDR CONNECT_ADDR SIZE MODE [SPORT]: hashed TCP transfer guest SRC -> guest DST
# with nettool.py; prints PASS/FAIL. Addresses as nettool.py takes them.
S=$(dirname "$(readlink -f "$0")")
src=$1 dst=$2 la=$3 ca=$4 size=$5 mode=$6 sport=${7:-0} port=${PORT:-5310}
NT=/mnt/hostfs/test-rx/guest/nettool.py
"$S/gssh" "$dst" "pkill -f '^python3 .*tcp-recv' ; rm -f /tmp/xfer.out; nohup python3 $NT tcp-recv '$la' $port > /tmp/xfer.out 2>&1 &
for i in \$(seq 100); do grep -q listening /tmp/xfer.out 2>/dev/null && exit 0; sleep 0.1; done; echo no listener; cat /tmp/xfer.out; exit 1" || exit 1
want=$("$S/gssh" "$src" "timeout ${TMO:-120} python3 $NT tcp-send '$ca' $port $size $mode $sport" 2>&1)
got=$("$S/gssh" "$dst" "for i in \$(seq 300); do [ \$(wc -l < /tmp/xfer.out) -ge 2 ] && break; sleep 0.1; done; tail -n 1 /tmp/xfer.out")
if [ -n "$want" ] && [ "$want" = "$got" ]; then echo "PASS $src->$dst $mode $size ($la): $got"; else echo "FAIL $src->$dst $mode $size ($la): sent '$want' received '$got'"; exit 1; fi
