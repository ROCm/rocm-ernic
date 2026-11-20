#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# One GPU-to-GPU devmem TCP case on stack 3.
#
#   case.sh LABEL PORT TXSRC RXSRC SIZE SENDS
#     TXSRC: udmabuf | gem-vram | gem-gtt | hip | hip-kernel | copy | zc
#     RXSRC: udmabuf | gem-vram | gem-gtt | hip | hip-kernel | plain
#     SIZE : both buffers, e.g. 64m (must be a page multiple)
#     SENDS: -n of the plan (sends of $LEN bytes; LEN=1m by default)
#
#   env: LEN, SEED (default random), PROBE=0|1 (default 1), SECS,
#        HOLD=0|1 (host-side check of the RECEIVER server's VRAM memfd),
#        EXTRA_TX, EXTRA_RX, RXG (receiver guest, default 2), TXG (default 1)
#
# Evidence: $EV/LABEL.txt, plus -rxprobe.txt and -txprobe.txt.
#
# The sender is devmem_tx_ext2 / hip_devmem_src2, which are NOT in this
# repository: they are devmem_tx_gpu.c and a HIP exporter with extra
# options, built in the guests' ~/gputx.  See ../../README.md.
set -uo pipefail
HERE=$(dirname "$(readlink -f "$0")")
# shellcheck source=tests/devmem/scripts/stack3/env.sh
. "$HERE/env.sh"
mkdir -p "$EV"

L=${1:?label}
PORT=${2:?port}
TXSRC=${3:?txsrc}
RXSRC=${4:?rxsrc}
SIZE=${5:?size}
N=${6:?sends}
LEN=${LEN:-1048576}
SEED=${SEED:-$((RANDOM * 32768 + RANDOM + 1))}
PROBE=${PROBE:-1}
HOLD=${HOLD:-0}
SECS=${SECS:-0}
EXTRA_TX=${EXTRA_TX:-}
EXTRA_RX=${EXTRA_RX:-}
RXG=${RXG:-2}
TXG=${TXG:-1}
RXIP=$ETH_SUBNET.$((10 + RXG))
OFF=0
CHUNK=0
GAP=0
ADV=$LEN
PLAN="-P $SEED -o $OFF -l $LEN -a $ADV -n $N"
GB=/home/ubuntu # the guest user's home, as the guest scripts spell it

bytes() {
    case "$1" in
    *m | *M) echo $((${1%[mM]} << 20)) ;;
    *g | *G) echo $((${1%[gG]} << 30)) ;;
    *k | *K) echo $((${1%[kK]} << 10)) ;;
    *) echo "$1" ;;
    esac
}
SZB=$(bytes "$SIZE")

RXBIN="$GB/e2e/devmem_rx_gpu -f $IF -s $RXIP -p $PORT -t 0 -q 1 -S $SIZE -B $SIZE $PLAN -v -W 90"
[ "$HOLD" = 1 ] && RXBIN="$RXBIN -R /tmp/$L-rec.txt -K /tmp/$L-go -k 180"
case $RXSRC in
udmabuf) RXCMD="sudo $RXBIN $EXTRA_RX" ;;
gem-vram) RXCMD="sudo $RXBIN -G /dev/dri/renderD128 $EXTRA_RX" ;;
gem-gtt) RXCMD="sudo $RXBIN -G /dev/dri/renderD128 -J $EXTRA_RX" ;;
hip) RXCMD="sudo $GB/e2e/hip_devmem_rx $SZB -- $RXBIN $EXTRA_RX" ;;
hip-kernel) RXCMD="sudo $GB/e2e/hip_devmem_rx -c $SZB -- $RXBIN $EXTRA_RX" ;;
plain) RXCMD="sudo $GB/gputx/devmem_tx_ext2 -r -s $RXIP -p $PORT -S $SIZE $PLAN -W 90" ;;
*)
    echo "unknown RXSRC $RXSRC" >&2
    exit 2
    ;;
esac

TXBIN="$GB/gputx/devmem_tx_ext2 -f $IF -s $RXIP -p $PORT -S $SIZE $PLAN -C 60 -t 90"
[ "$SECS" != 0 ] && TXBIN="$TXBIN -T $SECS"
case $TXSRC in
udmabuf) TXCMD="sudo $TXBIN $EXTRA_TX" ;;
gem-vram) TXCMD="sudo $TXBIN -G /dev/dri/renderD128 $EXTRA_TX" ;;
gem-gtt) TXCMD="sudo $TXBIN -G /dev/dri/renderD128 -J $EXTRA_TX" ;;
hip) TXCMD="sudo timeout -s KILL 900 $GB/gputx/hip_devmem_src2 $SZB $SEED -- $TXBIN $EXTRA_TX" ;;
hip-kernel) TXCMD="sudo timeout -s KILL 900 $GB/gputx/hip_devmem_src2 -k $SZB $SEED -- $TXBIN $EXTRA_TX" ;;
copy) TXCMD="sudo $TXBIN -m copy $EXTRA_TX" ;;
zc) TXCMD="sudo $TXBIN -m zc $EXTRA_TX" ;;
*)
    echo "unknown TXSRC $TXSRC" >&2
    exit 2
    ;;
esac

emu() { # guest index: the emulator counters that must stay at 0
    docker exec "$PROJECT-ernic-$1-1" cat "/run/ernic-stats/ernic-$1.stats" |
        grep -E '^ +(tx_dma_error|rx_dma_error|tx_desc_data_error|tx_desc_fetch_error|rx_desc_fetch_error|frames_rx_dropped|rx_queue_empty) '
}

place() { # guest index: where the dma-buf is while it is bound
    g "$1" - <<'EOS'
sudo sh -c '
echo "== dma_buf/bufinfo"; cat /sys/kernel/debug/dma_buf/bufinfo
echo "== amdgpu_gem_info (pinned/exported)"; grep -E "pin count|exported" /sys/kernel/debug/dri/1/amdgpu_gem_info
echo "== amdgpu_vm_info exported"; grep -E "^pid|exported" /sys/kernel/debug/dri/1/amdgpu_vm_info
echo "== KFD evicted_ms"; grep -H . /sys/class/kfd/kfd/proc/*/stats_*/evicted_ms 2>/dev/null
echo "== vram_mm"; head -4 /sys/kernel/debug/dri/1/amdgpu_vram_mm
echo "== page pools"; cat /proc/net/page_pool 2>/dev/null | head -5'
EOS
}

memfd_sha() { # guest-index offset length: hash the NIC server's VRAM mapping
    docker exec "$PROJECT-ernic-$1-1" sh -c \
        'F=/proc/1/fd/$(ls -l /proc/1/fd | awk "/rocjitsu-vram/{print \$9; exit}"); dd if=$F bs=4096 skip='$(($2 / 4096))' count='$((($3 + 4095) / 4096))' 2>/dev/null | sha256sum | cut -d" " -f1'
}

{
    echo "===== CASE $L  tx=$TXSRC rx=$RXSRC size=$SIZE sends=$N len=$LEN seed=$SEED  $(date -u +%FT%TZ)"
    echo "RX(\$ on r102-$RXG): $RXCMD"
    echo "TX(\$ on r102-$TXG): $TXCMD"
    echo "--- emulator counters before: ernic-$RXG (receiver)"
    emu "$RXG"
    echo "--- emulator counters before: ernic-$TXG (sender)"
    emu "$TXG"

    if [ "$PROBE" = 1 ]; then
        for gX in $RXG $TXG; do
            g "$gX" "sudo rm -f /tmp/$L-probe.txt; sudo nohup bpftrace -B line $GB/e2e/rx-probe.bt >/tmp/$L-probe.txt 2>&1 &
      for i in \$(seq 400); do sudo grep -qE 'Attach(ed|ing)' /tmp/$L-probe.txt 2>/dev/null && break; sleep 0.1; done
      sudo grep -qE 'Attach(ed|ing)' /tmp/$L-probe.txt && echo \"probe attached on guest $gX\" || { echo 'PROBE FAILED'; sudo cat /tmp/$L-probe.txt; }"
        done
        sleep 1
    fi

    echo "--- receiver r102-$RXG"
    g "$RXG" "sudo rm -f /tmp/$L-rx.log /tmp/$L-go /tmp/$L-rec.txt; echo 'E2E-CASE $L' | sudo tee /dev/kmsg >/dev/null
  nohup $RXCMD >/tmp/$L-rx.log 2>&1 &
  for i in \$(seq 900); do grep -qE 'RX listening|RESULT|FAILED' /tmp/$L-rx.log && break; sleep 0.1; done
  cat /tmp/$L-rx.log" || exit 1
    echo "--- receiver placement while bound (r102-$RXG)"
    place "$RXG"

    echo "--- sender r102-$TXG"
    g "$TXG" "echo 'E2E-CASE $L' | sudo tee /dev/kmsg >/dev/null; sudo rm -f /tmp/$L-tx.log
  nohup $TXCMD >/tmp/$L-tx.log 2>&1 &
  TX=\$!
  for i in \$(seq 300); do sudo grep -q '0000:01:00.0' /sys/kernel/debug/dma_buf/bufinfo && break; kill -0 \$TX 2>/dev/null || break; sleep 0.1; done
  sleep 1"
    echo "--- sender placement while bound (r102-$TXG)"
    place "$TXG"

    if [ "$HOLD" = 1 ]; then
        echo "--- waiting for the receiver to HOLD its last batch"
        g "$RXG" "for i in \$(seq 4500); do grep -q '^HOLD ' /tmp/$L-rx.log && break; sleep 0.2; done; grep '^HOLD ' /tmp/$L-rx.log"
        echo "--- host-side check of the RECEIVER's NIC server memory (ernic-$RXG)"
        g "$RXG" "sudo grep -m2 'VRAM-MAP' /tmp/$L-probe.txt" >"/tmp/$L-vrammap.txt" 2>&1
        cat "/tmp/$L-vrammap.txt"
        dma=$(sed -n 's/.*dma=\(0x[0-9a-f]*\).*/\1/p' "/tmp/$L-vrammap.txt" | head -1)
        nmap=$(grep -c VRAM-MAP "/tmp/$L-vrammap.txt")
        if [ -n "$dma" ] && [ "$nmap" = 1 ]; then
            g "$RXG" "tail -40 /tmp/$L-rec.txt" >"/tmp/$L-rec-tail.txt"
            held=$(g "$RXG" "grep -m1 '^HOLD ' /tmp/$L-rx.log" | sed -n 's/.*records=\([0-9]*\) held.*/\1/p')
            # the largest fragment among the last $held records
            read -r spos foff flen < <(tail -n "${held:-8}" "/tmp/$L-rec-tail.txt" |
                awk '$1=="D"{if($4+0>m){m=$4;s=$2;o=$3}}END{print s,o,m}')
            base=$((dma - 0x4000000000))
            want=$(python3 -I "$HERE/plan_bytes.py" "$SEED" "$SZB" "$OFF" "$LEN" "$CHUNK" "$GAP" "$ADV" "$spos" "$flen")
            echo "largest held fragment: stream_pos=$spos frag_offset=$foff frag_size=$flen"
            printf 'dma-buf VRAM base dma=%s -> BAR0 offset 0x%x; fragment at BAR0 offset 0x%x\n' \
                "$dma" "$base" "$((base + foff))"
            echo "expected sha256 of those $flen bytes (host-computed from the plan): $want"
            for i in 1 2 3; do
                got=$(docker exec "$PROJECT-ernic-$RXG-1" sh -c \
                    'F=/proc/1/fd/$(ls -l /proc/1/fd | awk "/rocjitsu-vram/{print \$9; exit}"); dd if=$F bs=1 skip='$((base + foff))' count='"$flen"' 2>/dev/null | sha256sum | cut -d" " -f1')
                echo "sample $i: receiver server /memfd:rocjitsu-vram: sha256 $got => $([ "$got" = "$want" ] && echo EQUALS-THE-SENT-BYTES || echo differs)"
            done
            echo "control, 64 KiB elsewhere in the same BAR: $(memfd_sha "$RXG" $((base + SZB + 1048576)) 65536)"
        else
            echo "no single VRAM-MAP line (count=$nmap): not a contiguous VRAM mapping; host memfd check skipped"
        fi
        g "$RXG" "touch /tmp/$L-go"
    fi

    echo "--- sender output"
    g "$TXG" "for i in \$(seq 4500); do pgrep -x devmem_tx_ext2 >/dev/null || pgrep -x hip_devmem_src2 >/dev/null || break; sleep 0.2; done
  pgrep -x devmem_tx_ext2 >/dev/null && { echo 'SENDER STILL RUNNING'; sudo pkill -x devmem_tx_ext2; }
  cat /tmp/$L-tx.log; echo \"tx_unreadable_drop: \$(sudo ethtool -S $IF | grep -w tx_unreadable_drop)\""
    echo "--- receiver output"
    g "$RXG" "for i in \$(seq 4500); do pgrep -x devmem_rx_gpu >/dev/null || break; sleep 0.2; done
  pgrep -x devmem_rx_gpu >/dev/null && { echo 'RECEIVER STILL RUNNING'; sudo pkill -x devmem_rx_gpu; }
  cat /tmp/$L-rx.log; echo \"rx_unreadable_drop/hds: \$(sudo ethtool -S $IF | grep -wE 'rx_unreadable_drop|rx_hds_split' | tr '\n' ' ')\""

    if [ "$PROBE" = 1 ]; then
        for gX in $RXG $TXG; do
            if [ "$gX" = "$RXG" ]; then tag=rxprobe; else tag=txprobe; fi
            echo "--- probe r102-$gX ($tag)"
            g "$gX" "sudo pkill -INT -x bpftrace; for i in \$(seq 150); do pgrep -x bpftrace >/dev/null || break; sleep 0.1; done; sudo cat /tmp/$L-probe.txt" |
                tee "$EV/$L-$tag.txt" |
                grep -vE '^$|WARNING: Return value discarded|delete\(@|~~~|^Attach'
        done
    fi
    echo "--- after: 0 dma-buf objects / page pools"
    for gX in $RXG $TXG; do
        g "$gX" "echo -n 'r102-$gX dma-buf objects: '; sudo grep -c '^[0-9]' /sys/kernel/debug/dma_buf/bufinfo || true; echo -n 'dmabuf page pools: '; grep -c dmabuf /proc/net/page_pool 2>/dev/null || echo 0"
    done
    echo "--- dmesg since the marker"
    for gX in $RXG $TXG; do
        echo "r102-$gX:"
        g "$gX" "sudo dmesg | sed -n '/E2E-CASE $L/,\$p' | grep -E 'BUG|WARNING|Oops|Call Trace|ionic|amdgpu|page_pool' | cut -c1-200 | head -15"
    done
    echo "--- emulator counters after: ernic-$RXG (receiver)"
    emu "$RXG"
    echo "--- emulator counters after: ernic-$TXG (sender)"
    emu "$TXG"
    echo "===== END $L $(date -u +%FT%TZ)"
} 2>&1 | tee "$EV/$L.txt"
grep -hE '^RESULT (rx|tx) ' "$EV/$L.txt" | sed "s/^/[$L] /"
