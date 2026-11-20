#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# One GPU-dma-buf devmem TX case on stack 1, with the placement evidence that
# decides whether the NIC read VRAM or system RAM.
#
#   run-vram-test.sh LABEL PORT SECONDS [EXTRA_SENDER_ARGS...]
#
# Sender: guest 1 (must be on 7.4.0-rc3-devmem-kasan-p2p with amdgpu probed
# and the patched ionic loaded).  Receiver: guest 2, plain TCP.
# Evidence goes to test/evidence/gpu-vram/<LABEL>-*.
set -euo pipefail
S=$(dirname "$(readlink -f "$0")")/..
T=${DEVMEM_ROOT:?set it to the work area}/test
E=$T/evidence/gpu-vram
LABEL=${1:?label}
PORT=${2:?port}
SECS=${3:?seconds}
shift 3
PLAN="-S 4m -P 1 -o 123 -l 100003 -a 7919"
mkdir -p "$E"

echo "## receiver (guest 2)"
"$S/gssh" 2 "rm -f /tmp/$LABEL-rx.log; nohup ~/gputx/devmem_tx_gpu -r -s 10.213.100.12 -p $PORT $PLAN -n 0 -W 40 >/tmp/$LABEL-rx.log 2>&1 &
for i in \$(seq 100); do ss -ltn | grep -q \":$PORT \" && break; sleep 0.1; done
ss -ltn | grep \":$PORT \" || { echo 'receiver did not listen'; exit 1; }"

echo "## sender + probes (guest 1)"
"$S/gssh" 1 - <<EOS
set -eu
mountpoint -q /mnt/hostfs || sudo mount /mnt/hostfs
sudo dmesg -C
rm -f /tmp/$LABEL-probe.txt /tmp/$LABEL-tx.log /tmp/$LABEL-live.txt
sudo bpftrace /mnt/hostfs/test/scripts/gpu/p2p-probe.bt >/tmp/$LABEL-probe.txt 2>&1 &
BT=\$!
for i in \$(seq 200); do grep -q 'Attached .* probes' /tmp/$LABEL-probe.txt && break; sleep 0.1; done
grep -q 'Attached .* probes' /tmp/$LABEL-probe.txt || { echo 'bpftrace did not attach'; cat /tmp/$LABEL-probe.txt; exit 1; }

~/gputx/devmem_tx_gpu -f rocm-ernic0 -s 10.213.100.12 -p $PORT $PLAN -T $SECS -G /dev/dri/renderD128 $* >/tmp/$LABEL-tx.log 2>&1 &
TX=\$!
# Capture placement while the binding is live.
sleep 2
{
  echo "### captured while the devmem TX binding is live"
  echo "== /sys/kernel/debug/dma_buf/bufinfo"
  sudo cat /sys/kernel/debug/dma_buf/bufinfo
  echo "== /sys/kernel/debug/dri/0/amdgpu_gem_info"
  sudo cat /sys/kernel/debug/dri/0/amdgpu_gem_info
  echo "== /sys/kernel/debug/dri/0/amdgpu_vram_mm"
  sudo cat /sys/kernel/debug/dri/0/amdgpu_vram_mm 2>&1 | head -20
  echo "== /sys/kernel/debug/dri/0/amdgpu_gtt_mm"
  sudo cat /sys/kernel/debug/dri/0/amdgpu_gtt_mm 2>&1 | head -20
} >/tmp/$LABEL-live.txt 2>&1
wait \$TX || true
sleep 1
sudo kill -INT \$BT
wait \$BT || true
echo "=== tx ==="; cat /tmp/$LABEL-tx.log
echo "=== dmesg ==="; sudo dmesg | tail -25
EOS

echo "## collecting"
"$S/gssh" 1 "cat /tmp/$LABEL-probe.txt" >"$E/$LABEL-probe.txt"
"$S/gssh" 1 "cat /tmp/$LABEL-live.txt" >"$E/$LABEL-live-placement.txt"
"$S/gssh" 1 "cat /tmp/$LABEL-tx.log" >"$E/$LABEL-tx.txt"
"$S/gssh" 2 "cat /tmp/$LABEL-rx.log" >"$E/$LABEL-rx.txt"
docker cp ionic-devmem-ernic-1-1:/run/ernic-stats/ernic-1.stats - 2>/dev/null | tar -xO >"$E/$LABEL-ernic1-stats.txt" || true
echo "## done: $E/$LABEL-*"
