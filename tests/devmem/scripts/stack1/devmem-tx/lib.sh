#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Library for the devmem TX matrix (source it). Host side; guests are
# reached with gssh. Guest 1 = sender (192.168.100.11), guest 2 = receiver.
T=${DEVMEM_ROOT:?set it to the work area}/test
S=$T/scripts
D=$S/devmem-tx
E=${E:-$T/evidence/tx-7.2.15}
IF=rocm-ernic0
# ALT=1: use 10.213.100.1N instead of 192.168.100.1N (see testbed.md section 11: another
# docker network on 192.168.96.0/20 makes the host masquerade the guests' 192.168.100.x frames)
# shellcheck disable=SC2034 # IP1 is for the scripts that source this
if [ -n "${ALT:-}" ]; then IP1=10.213.100.11; IP2=10.213.100.12; else IP1=192.168.100.11; IP2=192.168.100.12; fi
altaddr() { local n; for n in 1 2; do "$S/gssh" $n "ip -4 -o addr show dev $IF | grep -q 10.213.100.1$n || sudo ip addr add 10.213.100.1$n/24 dev $IF"; done; }
DT=/usr/local/bin/devmem_tx   # installed from /mnt/hostfs/test/bin by install.sh (do not run it over 9p: a rebuilt file can be served from stale cache)
G1() { "$S/gssh" 1 "$@"; }
G2() { "$S/gssh" 2 "$@"; }

# snap LABEL: counters of both guests and both emulators -> $SNAPDIR/LABEL.*
SNAPDIR=$(mktemp -d /tmp/devmem-tx-snap.XXXXXX)
guest_snap() { # N
    "$S/gssh" "$1" - <<'EOF2'
sudo ethtool -S rocm-ernic0 | awk -F: 'NR>1{gsub(/ /,"",$1); gsub(/ /,"",$2); print "ethtool."$1, $2}'
for f in tx_dropped tx_errors tx_packets rx_dropped rx_errors rx_packets; do echo "netdev.rocm-ernic0.$f $(cat /sys/class/net/rocm-ernic0/statistics/$f)"; done
for f in tx_dropped tx_packets tx_bytes; do echo "netdev.enp0s5.$f $(cat /sys/class/net/enp0s5/statistics/$f)"; done
echo "dma-api.error_count $(sudo cat /sys/kernel/debug/dma-api/error_count)"
echo "dmabuf.objects $(sudo awk '/^Total/{print $2}' /sys/kernel/debug/dma_buf/bufinfo)"
echo "dma-api.nr_total_entries $(sudo cat /sys/kernel/debug/dma-api/nr_total_entries)"
echo "dma-api.num_free_entries $(sudo cat /sys/kernel/debug/dma-api/num_free_entries)"
nstat -az 2>/dev/null | awk '/TcpRetransSegs|TCPFastRetrans|TCPTimeouts|TCPLossProbes|TcpOutRsts|TCPSlowStartRetrans|TcpInErrs|TcpInCsumErrors|TCPAbortOnTimeout/{print "tcp."$1, $2}'
echo "dmesg.splat_lines $(sudo dmesg | grep -v -E 'RCU lockdep checking is enabled|DMA-API: (preallocated|debugging enabled)' | grep -c -E 'WARNING|BUG|Oops|KASAN|UBSAN|lockdep|DMA-API|refcount_t|use-after-free|general protection|Call Trace|hw csum failure|leak|stalled|circular locking|inconsistent lock state|sleeping function|suspicious RCU|possible recursive locking|bad unlock balance|held lock freed')"
EOF2
}
emu_snap() { # N
    docker cp "ionic-devmem-ernic-$1-1:/run/ernic-stats/ernic-$1.stats" - | tar -xO |
        awk -F: '/^  [a-z_0-9]+ +:/{k=$1; gsub(/ /,"",k); v=$2; gsub(/ /,"",v); print "emu."k, v}' |
        grep -E 'emu\.(tx_dma_error|rx_dma_error|tx_desc_data_error|tx_desc_fetch_error|tx_queue_error|tx_queue_disabled|rx_queue_empty|rx_queue_error|rx_desc_fetch_error|rx_desc_data_error|rx_queue_disabled|frames_tx_ok|frames_tx_bad|frames_rx_ok|frames_rx_dropped|octets_tx_ok|reset_count) '
}
snap() {
    local n
    for n in 1 2; do
        { guest_snap $n; emu_snap $n; } >"$SNAPDIR/$1.$n" 2>&1
    done
}
# snapdiff A B: changed interesting counters
snapdiff() {
    local n
    for n in 1 2; do
        echo "--- counters changed, guest $n + emulator $n ($1 -> $2); unlisted = unchanged"
        join <(sort "$SNAPDIR/$1.$n") <(sort "$SNAPDIR/$2.$n") |
            awk '$2 != $3 { printf "  %-44s %12s -> %-12s (%+d)\n", $1, $2, $3, $3 - $2 }' |
            grep -E 'xdp|drop|unread|linear|err|tso|csum |dmabuf|tx_packets|rx_packets|frags|abort|miss|over|tcp\.|dma-api|dmesg|emu\.|enp0s5|tx_bytes|octets_tx_ok|tx_[0-9]_pkts' |
            grep -v -E 'frames_(rx|tx)_[0-9]|pri_' || true
        echo "  always shown: $(grep -h -E 'ethtool.tx_unreadable_drop |ethtool.hw_tx_dropped |netdev.rocm-ernic0.tx_dropped |emu.tx_dma_error |emu.tx_desc_data_error |emu.tx_desc_fetch_error |dma-api.error_count |dmesg.splat_lines |dmabuf.objects ' "$SNAPDIR/$2.$n" | tr '\n' ';')"
    done
}
# kernel log marker and dmesg since it
mark() {
    local n
    MARK="DEVMEM-TX-CASE $1 $(date +%s.%N)"
    for n in 1 2; do "$S/gssh" $n "echo '$MARK' | sudo tee /dev/kmsg >/dev/null"; done
}
dmesg_since() {
    local n
    for n in 1 2; do
        echo "--- dmesg guest $n since marker"
        "$S/gssh" $n "sudo dmesg | sed -n '/$MARK/,\$p' | tail -n +2 | cut -c1-240 | head -n ${DMESG_MAX:-40}"
    done
}
# start_rx PORT "PLAN" [extra]: receiver on guest 2, log /tmp/dtx-rx.log
start_rx() {
    G2 "rm -f /tmp/dtx-rx.log; nohup $DT -r -p $1 $2 ${3:-} >/tmp/dtx-rx.log 2>&1 &
        for i in \$(seq 100); do ss -ltnH 'sport = :$1' | grep -q . && exit 0; sleep 0.1; done; echo no listener >&2; exit 1"
}
# wait_rx [SECS]: wait for the receiver to exit, print its log
wait_rx() {
    G2 "for i in \$(seq $(( ${1:-60} * 5 ))); do pgrep -x devmem_tx >/dev/null || break; sleep 0.2; done
        if pgrep -x devmem_tx >/dev/null; then echo 'RECEIVER STILL RUNNING after ${1:-60}s; killing'; pkill -x devmem_tx; sleep 0.5; fi; cat /tmp/dtx-rx.log"
}
start_probe() {
    G1 "sudo rm -f /tmp/dtx-probe.out; sudo nohup bpftrace /mnt/hostfs/test/scripts/devmem-tx/probe.bt >/tmp/dtx-probe.out 2>&1 &
        for i in \$(seq 150); do grep -q -E 'Attach(ed|ing)' /tmp/dtx-probe.out 2>/dev/null && break; sleep 0.2; done; sleep 1; head -n 3 /tmp/dtx-probe.out"
}
stop_probe() {
    G1 "sudo pkill -INT -x bpftrace; for i in \$(seq 100); do pgrep -x bpftrace >/dev/null || break; sleep 0.2; done; sudo cat /tmp/dtx-probe.out" >"$SNAPDIR/probe.out"
    # the PFN map can have thousands of keys: summarize it, print the rest
    grep -v '^@devmem_frag_pfn\[' "$SNAPDIR/probe.out" | grep -v '^$'
    grep '^@devmem_frag_pfn\[' "$SNAPDIR/probe.out" | sed -E 's/^@devmem_frag_pfn\[([0-9]+), ([0-9]+)\].*/\1 \2/' | sort -n >"$SNAPDIR/probe.portpfns"
    echo "devmem frag DMA pages seen by the driver, by destination port: $(awk '{n[$1]++} END{for (p in n) printf "port %s: %d pages; ", p, n[p]}' "$SNAPDIR/probe.portpfns")"
}
# pfn_check PORT: every DMA page the driver used for a frag of an unreadable
# skb of this test's connection is a page of this test's dmabuf (sender ran
# with -F /tmp/dtx-pfns). Frags of other connections (an earlier test's
# orphan retransmitting from its own dmabuf) are listed, not judged.
pfn_check() {
    G1 "sudo cat /tmp/dtx-pfns" | while read -r h; do echo $((16#$h)); done | sort -n >"$SNAPDIR/dmabuf.pfns"
    awk -v p="$1" '$1 == p {print $2}' "$SNAPDIR/probe.portpfns" | sort -n >"$SNAPDIR/probe.pfns"
    local outside others
    outside=$(comm -23 "$SNAPDIR/probe.pfns" "$SNAPDIR/dmabuf.pfns" | wc -l)
    others=$(awk -v p="$1" '$1 != p' "$SNAPDIR/probe.portpfns" | wc -l)
    echo "PFN-CHECK port $1: dmabuf pages=$(wc -l <"$SNAPDIR/dmabuf.pfns") driver-frag pages=$(wc -l <"$SNAPDIR/probe.pfns") outside-the-dmabuf=$outside (frag pages of other connections: $others) => $(if [ ! -s "$SNAPDIR/probe.pfns" ]; then echo "N/A (the driver mapped no devmem frag for this port)"; elif [ "$outside" = 0 ]; then echo PASS; else echo FAIL; fi)"
}
feat() { # N : feature and link state line
    "$S/gssh" "$1" "echo \"guest $1: \$(uname -r) mtu \$(cat /sys/class/net/$IF/mtu) \$(ethtool -k $IF | grep -E '^(tx-checksumming|scatter-gather|tcp-segmentation-offload|generic-segmentation-offload):' | tr -s ' \n' ' ') rx-ring \$(ethtool -g $IF | awk '/^RX:/{n++; if(n==2)print \$2}')\""
}
# devmem_case NAME PORT "PLAN" ["SENDER EXTRA"] ["RX EXTRA"]: one run.
# Environment: RXWAIT (secs to wait for the receiver), CAP=1 (capture on the
# sender emulator's tap0 and on the receiver guest's netdev), HOOK (shell
# command run on the host HOOK_DELAY seconds after the sender started),
# TXIF/TXDST/TXOPTS to change the sender's -f/-s, SENDER_GUEST_PRE (run in guest 1 first).
devmem_case() {
    local name=$1 port=$2 plan=$3 sx=${4:-} rx=${5:-} dst=${TXDST:-$IP2}
    echo "===== CASE $name  $(date -u +%FT%TZ)"
    feat 1; feat 2
    mark "$name"; snap before
    start_rx "$port" "$plan" "$rx"
    if [ -n "${CAP:-}" ]; then
        cap_start 1 "$port"
        G2 "sudo rm -f /tmp/dtx-rx.pcap; sudo nohup tcpdump -i $IF -U -w /tmp/dtx-rx.pcap tcp port $port >/dev/null 2>&1 & sleep 1"
    fi
    start_probe
    echo "\$ sudo devmem_tx -f $IF -s $dst -p $port $plan -D -F /tmp/dtx-pfns $sx"
    if [ -n "${HOOK:-}" ]; then
        G1 "sudo $DT -f $IF -s $dst -p $port $plan -D -F /tmp/dtx-pfns $sx" >"$SNAPDIR/tx.out" 2>&1 &
        local txpid=$!
        sleep "${HOOK_DELAY:-4}"
        echo "--- hook at +${HOOK_DELAY:-4}s: $HOOK"
        eval "$HOOK" 2>&1 | sed 's/^/    hook: /'
        wait $txpid; local rc=$?
        cat "$SNAPDIR/tx.out"; echo "sender exit status: $rc"
    else
        G1 "sudo $DT -f $IF -s $dst -p $port $plan -D -F /tmp/dtx-pfns $sx" 2>&1 | tee "$SNAPDIR/tx.out"; echo "sender exit status: ${PIPESTATUS[0]}"
    fi
    wait_rx "${RXWAIT:-60}"
    echo "--- bpftrace probe (sender guest)"
    stop_probe
    pfn_check "$port"
    if [ -n "${CAP:-}" ]; then
        echo "--- captures"
        cap_stop 1 "$E/pcap/$name.tap0-emulator1.pcap"
        pcap_summary "$E/pcap/$name.tap0-emulator1.pcap" "$port"
        G2 "sudo pkill -INT -x tcpdump; sleep 0.5; sudo cat /tmp/dtx-rx.pcap" >"$E/pcap/$name.receiver-guest.pcap"
        pcap_summary "$E/pcap/$name.receiver-guest.pcap" "$port"
    fi
    sleep 1.2 # the emulator rewrites its stats file every second
    snap after; snapdiff before after
    dma_api_check
    dmesg_since
}
# DMA-API reports on the sender other than the known sg-segment ones at bind
dma_api_check() {
    local b a inbind
    b=$(awk '$1=="dma-api.error_count"{print $2}' "$SNAPDIR/before.1")
    a=$(awk '$1=="dma-api.error_count"{print $2}' "$SNAPDIR/after.1")
    inbind=$(awk '/DMA-API error_count during bind/{s+=$5} END{print s+0}' "$SNAPDIR/tx.out")
    echo "DMA-API-CHECK sender error_count delta=$((a - b)) of which at bind (sg segment > 64K)=$inbind other=$((a - b - inbind)) => $([ $((a - b - inbind)) = 0 ] && echo PASS || echo FAIL)"
}
# Host-side capture: pcapcap on tap0 inside emulator N's network namespace
# (what emulator N wrote to / read from the tap). cap_start N PORT; cap_stop N OUT.pcap
cap_start() {
    docker rm -f "ionic-devmem-cap-$1" >/dev/null 2>&1 || true
    docker create --cgroup-parent=system.slice --name "ionic-devmem-cap-$1" \
        --network "container:ionic-devmem-ernic-$1-1" --cap-drop ALL --cap-add NET_RAW --cap-add NET_ADMIN \
        -v "$T/bin:/cap:ro" --entrypoint /cap/pcapcap qemu-tool/ernic-tapsetup:1 tap0 /tmp/cap.pcap 900 "$2" >/dev/null
    docker start "ionic-devmem-cap-$1" >/dev/null
    for _ in $(seq 50); do docker logs "ionic-devmem-cap-$1" 2>&1 | grep -q capturing && break; sleep 0.1; done
    docker logs "ionic-devmem-cap-$1" 2>&1 | tail -n 1
}
cap_stop() {
    docker kill -s TERM "ionic-devmem-cap-$1" >/dev/null
    docker wait "ionic-devmem-cap-$1" >/dev/null
    docker logs "ionic-devmem-cap-$1" 2>&1 | tail -n 1
    docker cp "ionic-devmem-cap-$1:/tmp/cap.pcap" "$2"
    docker rm "ionic-devmem-cap-$1" >/dev/null
}
# pcap_summary FILE PORT: frames to/from PORT, how many carry TCP payload from the sender, payload bytes, all-zero payloads
pcap_summary() {
    python3 "$D/pcapsum.py" "$1" "$2"
}
