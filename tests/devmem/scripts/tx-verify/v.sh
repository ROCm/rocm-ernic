#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Verification harness (source it). Own tools: vtx (sender), vrx.py (receiver), vprobe.bt.
T=${DEVMEM_ROOT:?set it to the work area}/test; V=$T/evidence/tx-verify; G=$T/scripts/gssh
IF=rocm-ernic0
# shellcheck disable=SC2034 # IP1 is for the scripts that source this
IP1=10.213.100.11
IP2=10.213.100.12
g1() { "$G" 1 "$@"; }; g2() { "$G" 2 "$@"; }
vinstall() { local n; for n in 1 2; do "$G" $n 'sudo tee /usr/local/bin/vtx >/dev/null; sudo chmod 755 /usr/local/bin/vtx' <"$V/bin/vtx"; "$G" $n 'sudo tee /usr/local/bin/vrx.py >/dev/null; sudo chmod 755 /usr/local/bin/vrx.py' <"$V/src/vrx.py"; "$G" $n 'sudo tee /usr/local/bin/vprobe.bt >/dev/null' <"$V/src/vprobe.bt"; "$G" $n 'sha256sum /usr/local/bin/vtx /usr/local/bin/vrx.py /usr/local/bin/vprobe.bt'; done; sha256sum "$V/bin/vtx" "$V/src/vrx.py" "$V/src/vprobe.bt"; }
vsnap() { # N -> stdout "key value"
    # shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
    "$G" "$1" 'sudo ethtool -S rocm-ernic0 | awk -F: "NR>1{gsub(/ /,\"\",\$1); gsub(/ /,\"\",\$2); print \"eth.\"\$1, \$2}"; for f in tx_dropped tx_errors rx_dropped rx_errors; do echo "netdev.$f $(cat /sys/class/net/rocm-ernic0/statistics/$f)"; done; for f in tx_dropped tx_packets; do echo "enp0s5.$f $(cat /sys/class/net/enp0s5/statistics/$f)"; done; echo "dmaapi.error_count $(sudo cat /sys/kernel/debug/dma-api/error_count)"; echo "dmabuf.objects $(sudo awk "/^Total/{print \$2}" /sys/kernel/debug/dma_buf/bufinfo)"; nstat -az 2>/dev/null | awk "/TcpRetransSegs|TCPTimeouts|TcpOutRsts|TcpInCsumErrors|TcpInErrs|TCPZeroWindow|TCPFromZeroWindowAdv|TCPToZeroWindowAdv|TCPWantZeroWindowAdv|TCPAbortOnData|TCPAbortOnClose|TCPAbortOnTimeout/{print \"tcp.\"\$1, \$2}"; echo "tainted $(cat /proc/sys/kernel/tainted)"'
    docker cp "ionic-devmem-ernic-$1-1:/run/ernic-stats/ernic-$1.stats" - | tar -xO | awk -F: '/^  [a-z_0-9]+ +:/{k=$1; gsub(/ /,"",k); v=$2; gsub(/ /,"",v); print "emu."k, v}'
}
vdiff() { # fileA fileB
    join <(sort "$1") <(sort "$2") | awk '$2 != $3 { printf "    %-40s %14s -> %-14s (%+d)\n", $1, $2, $3, $3 - $2 }' | grep -E 'drop|unread|linear|err|eth\.tx_tso|eth\.tx_csum|dmabuf|abort|tcp\.|dmaapi|enp0s5.tx_dropped|tainted|eth\.tx_[0-9]_pkts|hw_tx|emu.frames_tx_ok|emu.frames_rx_ok|queue_empty|eth.tx_frags|stop|wake|emu.tx_tso|emu.tx_csum|emu.*(bad|disabled)' | grep -v -E 'frames_(rx|tx)_[0-9]|pri_' || true
}
SPLAT='WARNING|BUG|Oops|KASAN|UBSAN|DMA-API|refcount_t|use-after-free|general protection|Call Trace|hw csum failure|circular locking|inconsistent lock state|sleeping function|suspicious RCU|recursive locking|bad unlock|held lock freed|kmemleak|page_pool|leak'
# vcase NAME PORT "RX ARGS" "TX OPTS" SPEC...   env: HOOK (host shell, run HOOK_DELAY s after sender start), TXDST, PRE1 (guest-1 cmd before), NOPROBE=1, RXWAIT, TXIF
vcase() {
    local name=$1 port=$2 rxa=$3 txo=$4; shift 4
    local dst=${TXDST:-$IP2} tmp; tmp=$(mktemp -d /tmp/vcase.XXXXXX)
    echo "===== CASE $name  $(date -u +%FT%TZ)"
    local n mark; mark="VERIFY-CASE $name $(date +%s.%N)"
    for n in 1 2; do "$G" $n "echo '$mark' | sudo tee /dev/kmsg >/dev/null; echo \"guest $n: \$(uname -r) mtu \$(cat /sys/class/net/$IF/mtu) \$(ethtool -k $IF | grep -E '^(tx-checksumming|scatter-gather|tcp-segmentation-offload|generic-segmentation-offload):' | tr -s ' \n' ' ') dmabufs=\$(sudo awk '/^Total/{print \$2}' /sys/kernel/debug/dma_buf/bufinfo)\""; done
    for n in 1 2; do vsnap $n >"$tmp/before.$n"; done
    g2 "rm -f /tmp/vrx.log; nohup python3 /usr/local/bin/vrx.py $port $rxa >/tmp/vrx.log 2>&1 & for i in \$(seq 100); do grep -q listening /tmp/vrx.log 2>/dev/null && exit 0; sleep 0.1; done; echo NO LISTENER; exit 1"
    if [ -z "${NOPROBE:-}" ]; then
        g1 "sudo rm -f /tmp/vprobe.out; sudo nohup bpftrace /usr/local/bin/vprobe.bt >/tmp/vprobe.out 2>&1 & for i in \$(seq 300); do grep -q -E 'Attach(ed|ing)' /tmp/vprobe.out 2>/dev/null && break; sleep 0.2; done; sleep 1; head -n 2 /tmp/vprobe.out"
    fi
    [ -n "${PRE1:-}" ] && { echo "--- pre (guest 1): $PRE1"; g1 "$PRE1" 2>&1 | sed 's/^/    /'; }
    echo "\$ sudo vtx -e /tmp/vtx.exp $txo ${TXIF:-$IF} $dst $port $*"
    if [ -n "${HOOK:-}" ]; then
        g1 "sudo rm -f /tmp/vtx.exp; sudo vtx -e /tmp/vtx.exp $txo ${TXIF:-$IF} $dst $port $*" >"$tmp/tx.out" 2>&1 &
        local pid=$!
        sleep "${HOOK_DELAY:-3}"
        echo "--- hook at +${HOOK_DELAY:-3}s: $HOOK"
        eval "$HOOK" 2>&1 | sed 's/^/    hook: /'
        wait $pid; echo "sender exit status: $?" >>"$tmp/tx.out"
    else
        g1 "sudo rm -f /tmp/vtx.exp; sudo vtx -e /tmp/vtx.exp $txo ${TXIF:-$IF} $dst $port $*" >"$tmp/tx.out" 2>&1; echo "sender exit status: $?" >>"$tmp/tx.out"
    fi
    cat "$tmp/tx.out"
    g2 "for i in \$(seq $(( ${RXWAIT:-30} * 5 ))); do grep -q RESULT /tmp/vrx.log && break; sleep 0.2; done; grep -q RESULT /tmp/vrx.log || { echo 'vrx: NO RESULT after ${RXWAIT:-30}s; killing receiver'; pkill -f vrx.py; }; cat /tmp/vrx.log" | tee "$tmp/rx.out"
    local rxbytes rxsha zr txbytes expsha
    rxbytes=$(sed -n 's/.*RESULT bytes=\([0-9]*\).*/\1/p' "$tmp/rx.out"); rxsha=$(sed -n 's/.*sha256=\([0-9a-f]*\).*/\1/p' "$tmp/rx.out"); zr=$(sed -n 's/.*zero_runs_of_32=\([0-9]*\).*/\1/p' "$tmp/rx.out")
    txbytes=$(sed -n 's/.*RESULT [a-zA-Z-]* sendmsg_calls=[0-9]* bytes=\([0-9]*\).*/\1/p' "$tmp/tx.out")
    if [ -n "$rxbytes" ]; then
        expsha=$(g1 "sudo head -c $rxbytes /tmp/vtx.exp | sha256sum | cut -d' ' -f1; echo expected-file-bytes=\$(sudo stat -c %s /tmp/vtx.exp) >&2" 2>"$tmp/expsz")
        echo "VERDICT $name: sender-accepted=${txbytes:-?} $(cat "$tmp/expsz") receiver-got=$rxbytes sha256(expected[0:$rxbytes])=$expsha receiver-sha256=$rxsha zero_runs=$zr => $([ "$expsha" = "$rxsha" ] && [ "$zr" = 0 ] && echo DATA-OK || echo DATA-CORRUPT) $([ "$rxbytes" = "${txbytes:-x}" ] && echo COMPLETE || echo "PARTIAL(prefix)")"
    else
        echo "VERDICT $name: NO RECEIVER RESULT"
    fi
    if [ -z "${NOPROBE:-}" ]; then
        echo "--- vprobe (sender guest)"
        g1 "sudo pkill -INT -x bpftrace; for i in \$(seq 150); do pgrep -x bpftrace >/dev/null || break; sleep 0.2; done; sudo cat /tmp/vprobe.out" | grep -v '^$' | grep -v -E '^\[[0-9]+, [0-9]+\) +0 \||While loops|while \(|~~~|HINT'
    fi
    sleep 1.3
    for n in 1 2; do vsnap $n >"$tmp/after.$n"; echo "--- counters changed guest/emulator $n:"; vdiff "$tmp/before.$n" "$tmp/after.$n"; echo "    now: $(grep -h -E '^(eth.tx_unreadable_drop|netdev.tx_dropped|emu.tx_dma_error|emu.tx_desc_data_error|emu.tx_desc_fetch_error|dmaapi.error_count|dmabuf.objects|tainted) ' "$tmp/after.$n" | tr '\n' ';')"; done
    local eb ea ib; eb=$(awk '$1=="dmaapi.error_count"{print $2}' "$tmp/before.1"); ea=$(awk '$1=="dmaapi.error_count"{print $2}' "$tmp/after.1"); ib=$(sed -n 's/.*DMA-API error_count during bind: \([0-9-]*\).*/\1/p' "$tmp/tx.out" | head -1)
    echo "DMA-API sender error_count delta=$((ea - eb)), at bind=${ib:-?}, other=$((ea - eb - ${ib:-0}))"
    for n in 1 2; do echo "--- dmesg guest $n since marker ($( "$G" $n "sudo dmesg | sed -n '/$mark/,\$p' | tail -n +2 | grep -c -E '$SPLAT'" ) splat lines):"; "$G" $n "sudo dmesg | sed -n '/$mark/,\$p' | tail -n +2 | cut -c1-200 | head -n ${DMESG_MAX:-25}"; done
    cp "$tmp/tx.out" "$tmp/rx.out" /dev/null 2>/dev/null; echo
}

feat_reset() { local n; for n in 1 2; do "$G" $n "sudo ethtool -K $IF tx on sg on >/dev/null 2>&1; sudo ethtool -K $IF tso on gso on gro on >/dev/null 2>&1; sudo ip link set $IF mtu 1500; ip -4 -o addr show dev $IF | grep -q 10.213.100.1$n || sudo ip addr add 10.213.100.1$n/24 dev $IF; ethtool -k $IF | grep -E '^(tx-checksumming|scatter-gather|tcp-segmentation-offload|generic-segmentation-offload):' | tr -s ' \n' ' '; echo"; done; }
wait_dmabuf0() { local i; for i in $(seq "${1:-120}"); do [ "$(g1 "sudo awk '/^Total/{print \$2}' /sys/kernel/debug/dma_buf/bufinfo")" = 0 ] && { echo "dma-buf objects on guest 1: 0 after ${i}s"; return 0; }; sleep 1; done; echo "dma-buf objects on guest 1 STILL NONZERO after ${1:-120}s"; return 1; }
