#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# D1: concurrent devmem senders (own dmabuf + binding each) on different Tx queues, plus a copy-mode TCP stream.
# shellcheck source=tests/devmem/scripts/tx-verify/v.sh
. "$(dirname "$(readlink -f "$0")")/v.sh"
feat_reset >/dev/null; wait_dmabuf0 90
N=${N:-4}
echo "===== CASE D1-$N-concurrent-devmem-senders  $(date -u +%FT%TZ)"
mark="VERIFY-CASE D1 $(date +%s.%N)"
for n in 1 2; do "$G" $n "echo '$mark' | sudo tee /dev/kmsg >/dev/null"; done
tmp=$(mktemp -d /tmp/vd1.XXXXXX)
for n in 1 2; do vsnap $n >"$tmp/before.$n"; done
g2 "for i in \$(seq $N); do p=\$((6300+i)); rm -f /tmp/vrx.\$p.log; nohup python3 /usr/local/bin/vrx.py \$p >/tmp/vrx.\$p.log 2>&1 & done; sleep 1.5; grep -c listening /tmp/vrx.63*.log | tr '\n' ' '"
g1 "sudo rm -f /tmp/vprobe.out; sudo nohup bpftrace /usr/local/bin/vprobe.bt >/tmp/vprobe.out 2>&1 & for i in \$(seq 300); do grep -q -E 'Attach(ed|ing)' /tmp/vprobe.out 2>/dev/null && break; sleep 0.2; done; sleep 1"
g1 "for q in 0 1 2 3; do printf %x \$((1<<q)) | sudo tee /sys/class/net/rocm-ernic0/queues/tx-\$q/xps_cpus >/dev/null; done; grep . /sys/class/net/rocm-ernic0/queues/tx-*/xps_cpus | tr '\n' ' '"; echo
# each sender pinned to its own CPU (XPS maps CPUs to queues), different offsets/iovec shapes
g1 "sudo rm -f /tmp/vtx.63*; for i in \$(seq $N); do p=\$((6300+i)); sudo nohup sh -c \"taskset -c \$((i-1)) vtx -e - -S 8m -T 15 rocm-ernic0 $IP2 \$p d:\$((i*1001)):\$((500000+i*333)):\$((i%2 ? 0 : 3000)):\$((i%2 ? 0 : 97)):\$((7919*i)) 2>/tmp/vtx.\$p.log | sha256sum >/tmp/vtx.\$p.sha\" >/dev/null 2>&1 & done; wait; for i in \$(seq $N); do p=\$((6300+i)); echo \"sender \$p: \$(grep -E 'RESULT|FAILED|bound|expected' /tmp/vtx.\$p.log | tr '\n' ' ') expected-stream-sha256=\$(cut -d' ' -f1 /tmp/vtx.\$p.sha)\"; done" | tee "$tmp/tx.out"
sleep 2
for i in $(seq "$N"); do p=$((6300+i))
    rx=$(g2 "grep RESULT /tmp/vrx.$p.log"); echo "receiver $p: $rx"; rb=$(sed -n 's/.*RESULT bytes=\([0-9]*\).*/\1/p' <<<"$rx"); rs=$(sed -n 's/.*sha256=\([0-9a-f]*\).*/\1/p' <<<"$rx"); zr=$(sed -n 's/.*zero_runs_of_32=\([0-9]*\).*/\1/p' <<<"$rx")
    es=$(sed -n "s/^sender $p: .*expected-stream-sha256=\([0-9a-f]*\).*/\1/p" "$tmp/tx.out"); tb=$(sed -n "s/^sender $p: .*sendmsg_calls=[0-9]* bytes=\([0-9]*\).*/\1/p" "$tmp/tx.out")
    echo "VERDICT D1 port $p: sender-accepted=$tb receiver-got=$rb expected-sha=$es receiver-sha=$rs zero_runs=$zr => $([ -n "$es" ] && [ "$es" = "$rs" ] && [ "$zr" = 0 ] && [ "$tb" = "$rb" ] && echo DATA-OK COMPLETE || echo DATA-MISMATCH)"
done
echo "--- vprobe"; g1 "sudo pkill -INT -x bpftrace; for i in \$(seq 150); do pgrep -x bpftrace >/dev/null || break; sleep 0.2; done; sudo cat /tmp/vprobe.out" | grep -E "unreadable_skbs|readable_skbs|frag_netmem|unreadable_gso|bind|free|dropped|helper"
sleep 1.3
for n in 1 2; do vsnap $n >"$tmp/after.$n"; echo "--- counters changed guest/emulator $n:"; vdiff "$tmp/before.$n" "$tmp/after.$n"; echo "    now: $(grep -h -E '^(eth.tx_unreadable_drop|netdev.tx_dropped|emu.tx_dma_error|emu.tx_desc_data_error|emu.tx_desc_fetch_error|dmaapi.error_count|dmabuf.objects|tainted) ' "$tmp/after.$n" | tr '\n' ';')"; done
for n in 1 2; do echo "--- dmesg guest $n since marker ($( "$G" $n "sudo dmesg | sed -n '/$mark/,\$p' | tail -n +2 | grep -c -E '$SPLAT'" ) splat lines):"; "$G" $n "sudo dmesg | sed -n '/$mark/,\$p' | tail -n +2 | cut -c1-200 | head -n 25"; echo "    DMA-API lines other than the bind-time sg-segment report: $("$G" $n "sudo dmesg | sed -n '/$mark/,\$p' | grep 'DMA-API' | grep -v -c 'sg segment longer'"); WARNING lines not from debug_dma_map_sg: $("$G" $n "sudo dmesg | sed -n '/$mark/,\$p' | grep 'WARNING' | grep -v -c 'debug_dma_map_sg'"); KASAN/BUG lines: $("$G" $n "sudo dmesg | sed -n '/$mark/,\$p' | grep -c -E 'KASAN|BUG:|Oops|circular locking|refcount_t|sleeping function'")"; done
g1 "sudo rm -f /tmp/vtx.63*"
g1 "for q in 0 1 2 3; do echo 0 | sudo tee /sys/class/net/rocm-ernic0/queues/tx-\$q/xps_cpus >/dev/null; done"
echo "DONE d1"
