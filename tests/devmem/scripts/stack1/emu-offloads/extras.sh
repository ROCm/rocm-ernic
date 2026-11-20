#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# extras.sh OUTFILE: iperf3 (-Z sendfile, UDP), the kernel selftest
# msg_zerocopy (built in the guest from this kernel flavour's source), RDMA
# rping, and an MTU-limit probe, in both directions with default offloads.
# Not payload-verified (iperf3, msg_zerocopy count bytes); the hashed
# transfers are in run.py. Counters are printed as before/after deltas.
set -uo pipefail
S=${DEVMEM_ROOT:?set it to the work area}/test/scripts
SRC=${KSRC:-/mnt/hostfs/test/kbuild/7.2.15/src}
both() { for n in 1 2; do "$S/gssh" $n - <<<"$1"; done; }
both "sudo ethtool -K rocm-ernic0 tx on; sudo ethtool -K rocm-ernic0 sg on; sudo ethtool -K rocm-ernic0 tso on gso on gro on >/dev/null
sudo ip link set rocm-ernic0 mtu 1500
pgrep -x iperf3 >/dev/null || iperf3 -s -D
for i in \$(seq 50); do ss -ltnH 'sport = :5201' | grep -q . && break; sleep 0.1; done
if [ ! -x /tmp/msg_zerocopy ]; then gcc -O2 -I $SRC/tools/testing/selftests -o /tmp/msg_zerocopy $SRC/tools/testing/selftests/net/msg_zerocopy.c 2>&1 | tail -5; fi" >/dev/null 2>&1
{
    echo "== $(date '+%F %T') server: $(cat "${DEVMEM_ROOT:?set it to the work area}/test/stack/server-binary.path" 2>/dev/null || echo image)"
    for src in 1 2; do
        dst=$((3 - src)); a4=192.168.100.1$dst; a6=fd00:100::1$dst; s4=192.168.100.1$src; s6=fd00:100::1$src
        echo "---- devmem-$src -> devmem-$dst ($("${S}"/gssh $src 'uname -r; ls /sys/module | grep -c "^ionic_rdma$"' | tr '\n' ' ')ionic_rdma modules)"
        "$S/gssh" $src - <<EOS
nstat -n; ethtool -S rocm-ernic0 > /tmp/eth.before
ip() { python3 -c '
import sys,json
d=json.load(sys.stdin)["end"]; tag=sys.argv[1]
if "retransmits" in d.get("sum_sent", {}):
    print(tag, round(d["sum_received"]["bits_per_second"]/1e6), "Mbit/s received; sender retransmits", d["sum_sent"]["retransmits"])
else:
    s=d["sum"]; print(tag, round(s["bits_per_second"]/1e6,1), "Mbit/s; datagrams", s["packets"], "lost", s["lost_packets"])' "\$1"; }
iperf3 -c $a4 -t 10 -Z -J | ip "iperf3 -Z tcp4"
iperf3 -c $a6 -t 10 -Z -J | ip "iperf3 -Z tcp6"
iperf3 -c $a4 -t 5 -Z -P 4 -J | ip "iperf3 -Z -P4 tcp4"
iperf3 -c $a4 -u -b 50M -l 1400 -t 5 -J | ip "iperf3 -u 50M l=1400 v4"
iperf3 -c $a6 -u -b 50M -l 1400 -t 5 -J | ip "iperf3 -u 50M l=1400 v6"
iperf3 -c $a4 -u -b 50M -l 8000 -t 5 -J | ip "iperf3 -u 50M l=8000 (fragments) v4"
echo "-- nstat (sender) after iperf3:"; nstat | grep -E 'TcpRetransSegs|TCPLossProbes|TCPDSACKRecv |TCPTimeouts|TCPFastRetrans|InCsumErrors|TCPLostRetransmit' || echo "(none of the retransmit/csum counters moved)"
EOS
        for fam in 4 6; do
            if [ $fam = 4 ]; then sa=$s4; da=$a4; else sa=$s6; da=$a6; fi
            "$S/gssh" $dst "nohup /tmp/msg_zerocopy -$fam -C 2 -S $sa -D $da -r tcp >/tmp/mzc-rx.out 2>&1 & for i in \$(seq 50); do ss -ltnH | grep -q ':8000' && break; sleep 0.1; done"
            "$S/gssh" $src "/tmp/msg_zerocopy -$fam -C 3 -S $sa -D $da -t 2 -z -Z 1 tcp; echo \"msg_zerocopy -$fam tx exit=\$?\""
            "$S/gssh" $dst "for i in \$(seq 100); do pgrep -x msg_zerocopy >/dev/null || break; sleep 0.1; done; cat /tmp/mzc-rx.out"
        done
        "$S/gssh" $src "nstat | grep -E 'TcpRetransSegs|TCPLossProbes|TCPDSACKRecv |TCPTimeouts|TCPFastRetrans|InCsumErrors' || echo '(msg_zerocopy: no retransmit/csum counter moved)'
ethtool -S rocm-ernic0 > /tmp/eth.after; paste /tmp/eth.before /tmp/eth.after | awk '{d=\$4-\$2} \$1 ~ /^(tx_tso|tx_csum|tx_csum_none|hw_tx_dropped|hw_tx_aborted_errors|tx_[0-9]_(linearize|frags|dma_map_err)):\$/ && d {printf \"  %s +%d\\n\", \$1, d}'"
        if "$S/gssh" $src 'test -e /sys/module/ionic_rdma'; then
            "$S/gssh" $dst "nohup rping -s -a $a4 -C 20 -S 4096 >/tmp/rping-s.out 2>&1 & for i in \$(seq 50); do rdma res show cm_id 2>/dev/null | grep -q LISTEN && break; sleep 0.1; done"
            "$S/gssh" $src "rping -c -a $a4 -C 20 -S 4096 -v | awk '{l=length(\$0)} END{print NR, \"rping validated ping-data lines of 4096 bytes; last line length\", l}'; echo \"rping client exit=\${PIPESTATUS[0]}\""
            "$S/gssh" $dst "for i in \$(seq 50); do pgrep -x rping >/dev/null || break; sleep 0.1; done; echo \"rping server output: \$(wc -l </tmp/rping-s.out) lines; \$(tail -1 /tmp/rping-s.out | cut -c1-80)\""
        else
            echo "RDMA: ionic_rdma not loaded on this kernel: NOT RUN"
        fi
    done
    echo "---- MTU limit: 9198 accepted, 9199 refused?"
    "$S/gssh" 1 'sudo ip link set rocm-ernic0 mtu 9198 && echo "mtu 9198 ok"; sudo ip link set rocm-ernic0 mtu 9199; echo "mtu 9199 rc=$?"; sudo ip link set rocm-ernic0 mtu 1500'
    echo "---- emulator error counters now"
    for n in 1 2; do docker cp ionic-devmem-ernic-$n-1:/run/ernic-stats/ernic-$n.stats - | tar -xO | grep -E 'tx_desc_data_error|tx_dma_error|tx_desc_fetch_error|tx_queue_error|frames_tx_bad|rx_dma_error' | tr -s ' ' | tr '\n' ';'; echo " (ernic-$n)"; done
} 2>&1 | tee "$1"
