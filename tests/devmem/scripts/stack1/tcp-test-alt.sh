#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Same as tcp-test.sh, but over the 10.213.100.1N addresses that work around
# the host docker-network clash of finding F9 (192.168.100.x is rewritten by
# stack 2's MASQUERADE rule). Adds the addresses if they are missing.
#   tcp-test-alt.sh [SIZE_MB]      (default 64)
set -euo pipefail
S=$(dirname "$(readlink -f "$0")")
MB=${1:-64}
IP1=10.213.100.11 IP2=10.213.100.12 PORT=5311
ip_of() { [ "$1" = 1 ] && echo $IP1 || echo $IP2; }
for n in 1 2; do
    "$S/gssh" $n "ip addr show rocm-ernic0 | grep -q 10.213.100.1$n || sudo ip addr add 10.213.100.1$n/24 dev rocm-ernic0"
    echo "== devmem-$n: $("$S/gssh" $n 'uname -r; ethtool -i rocm-ernic0 | sed -n 1,2p' | tr '\n' ' ')"
done
rc=0
for src in 1 2; do
    dst=$((3 - src))
    echo "== ping devmem-$src -> devmem-$dst ($(ip_of $dst))"
    "$S/gssh" $src "ping -c 5 -i 0.2 -W 2 -I rocm-ernic0 $(ip_of $dst)" | tail -n 3 || rc=1
    echo "== TCP ${MB} MiB devmem-$src -> devmem-$dst"
    want=$("$S/gssh" $src "head -c $((MB * 1048576)) /dev/urandom > /tmp/tcp-test.bin; sha256sum < /tmp/tcp-test.bin")
    "$S/gssh" $dst "rm -f /tmp/tcp-test.sum; nohup sh -c 'socat -u TCP-LISTEN:$PORT,bind=$(ip_of $dst),reuseaddr - | sha256sum > /tmp/tcp-test.sum.tmp; mv /tmp/tcp-test.sum.tmp /tmp/tcp-test.sum' >/dev/null 2>&1 &
        for i in \$(seq 100); do ss -ltnH 'sport = :$PORT' | grep -q . && exit 0; sleep 0.1; done; echo no listener >&2; exit 1"
    "$S/gssh" $src "/usr/bin/time -f '%e s' socat -u FILE:/tmp/tcp-test.bin TCP:$(ip_of $dst):$PORT,bind=$(ip_of $src)"
    got=$("$S/gssh" $dst "for i in \$(seq 600); do [ -e /tmp/tcp-test.sum ] && break; sleep 0.1; done; cat /tmp/tcp-test.sum")
    echo "sent     $want"
    echo "received $got"
    if [ "$want" = "$got" ] && [ -n "$want" ]; then echo "PASS $src->$dst"; else echo "FAIL $src->$dst"; rc=1; fi
done
exit $rc
