#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# shape-tcp.sh [MB]: run tcp-test.sh with the skb-shape bpftrace on both guests, print counters.
S=$(dirname "$(readlink -f "$0")")
for n in 1 2; do "$S/gssh" $n "sudo pkill bpftrace; nohup sudo bpftrace /mnt/hostfs/test-rx/guest/skbshape.bt 14 > /tmp/skbshape.txt 2>&1 &
for i in \$(seq 150); do grep -q Attach /tmp/skbshape.txt 2>/dev/null && break; sleep 0.2; done; head -n 1 /tmp/skbshape.txt"; done
for n in 1 2; do echo "--- counters before rx-$n: $("$S/counters.sh" $n)"; done
"$S/tcp-test.sh" "${1:-64}" 2>&1 | grep -E "PASS|FAIL|sent|received|loss"
for n in 1 2; do echo "--- counters after rx-$n: $("$S/counters.sh" $n)"; done
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
for n in 1 2; do echo "--- skb shape rx-$n"; "$S/gssh" $n 'for i in $(seq 100); do pgrep bpftrace >/dev/null || break; sleep 0.2; done; cat /tmp/skbshape.txt'; done
"$S/splat.sh"
