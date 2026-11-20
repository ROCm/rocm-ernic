#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# final.sh: closing end-to-end run on fresh boots. Receiver guest 1 (one queue).
# shellcheck source=tests/devmem/scripts/stack2/tdlib.sh
. "$(dirname "$(readlink -f "$0")")/tdlib.sh"
"$S/td-setup.sh" | tr '\n' ' '; echo
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
for n in 1 2; do g $n 'echo "rx-'$n': $(uname -r) ionic taint=$(cat /sys/module/ionic/taint) rdma=$(rdma link | head -n 1 | cut -c1-60) split=$(ethtool -g rocm-ernic0 | grep -i "data split" | tr -s "\t" " ")"'; done
hdr "A: plain TCP sender (guest 2) -> devmem RX (guest 1), 64 MiB, -v 7 -L"
g 1 "nohup sudo bpftrace /mnt/hostfs/test-rx/guest/skbshape.bt 20 > /tmp/skbshape.txt 2>&1 &
for i in \$(seq 150); do grep -q Attach /tmp/skbshape.txt 2>/dev/null && break; sleep 0.2; done"
echo "   counters before: $("$S/counters.sh" 1)"; first_light 67108864; echo "   counters after: $("$S/counters.sh" 1)"
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
g 1 'for i in $(seq 150); do pgrep bpftrace >/dev/null || break; sleep 0.2; done; grep -E "headlen|unreadable|ip_summed" /tmp/skbshape.txt'
hdr "B: devmem TX (ncdevmem client, guest 2) -> devmem RX (guest 1), 700000 bytes in 100000 sends"
"$S/devmem-rx.sh" start 1 | tail -n 1
g 2 "$PAT | head -c 700000 > /tmp/pat700k.bin; sudo sh -c 'timeout 300 ncdevmem -f rocm-ernic0 -s $IP1 -p 5201 < /tmp/pat700k.bin > /tmp/nctx.out 2> /tmp/nctx.err; echo exit=\$? >> /tmp/nctx.err'; echo \"tx: sendmsg=\$(grep -c sendmsg_ret /tmp/nctx.err) completions=\$(grep -c 'tx complete' /tmp/nctx.err) \$(grep -E 'tx ok|exit=' /tmp/nctx.err | tr '\n' ' ')\"; ethtool -S rocm-ernic0 | grep -E 'tx_unreadable_drop:' | tr -s ' '"
"$S/devmem-rx.sh" wait 1 600
hdr "C: devmem TX with 3000-byte iovec chunks (-z 3000) is not applicable to 7-byte lines; plain large TSO sender with TCP_NODELAY small writes -> devmem RX"
"$S/devmem-rx.sh" start 1 | tail -n 1
g 2 "$PAT | head -c 2100000 | socat -u - TCP:$IP1:5201,nodelay,sndbuf=4096; echo sender rc=\$?"; "$S/devmem-rx.sh" wait 1 300
wait_no_dmabuf_pool 1
hdr "end state"; "$S/splat.sh"
for n in 1 2; do
    echo "   lockdep rx-$n: $(g $n "sudo grep debug_locks /proc/lockdep_stats | tr -s ' '")"
    # shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
    g $n 'echo "   rx-'$n' this boot: stalled-pool=$(sudo journalctl -k -b --no-pager | grep -c -E "stalled pool|page_pool_release_retry") KASAN=$(sudo journalctl -k -b --no-pager | grep -c "BUG: KASAN") lockdep-reports=$(sudo journalctl -k -b --no-pager | grep -c -E "possible circular|possible recursive|inconsistent lock state|suspicious RCU") hw-csum-failure=$(sudo journalctl -k -b --no-pager | grep -c "csum failure")"'; done
pools 1
