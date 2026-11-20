#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Leave the stack as found: 7.4-rc3-kasan + dmtx74-1; final sanity incl. a DMA map recount.
# shellcheck source=tests/devmem/scripts/tx-verify/v.sh
. "$(dirname "$(readlink -f "$0")")/v.sh"
S=$T/scripts
"$S"/install-kernel.sh 7.4-rc3 2>&1 | tail -4
"$S"/ionic-mod.sh load dmtx74-1 2>&1 | tail -2
feat_reset
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
for n in 1 2; do "$G" $n 'echo "guest: $(uname -r) taint [$(cat /sys/module/ionic/taint)] loaded ionic build-id $(sudo xxd -p /sys/module/ionic/notes/.note.gnu.build-id | tr -d "\n" | cut -c33-) dmtx74-1 file:$(readelf -n ~/ionic-mod/dmtx74-1/ionic.ko | grep "Build ID" | tr -s " ")"'; "$G" $n 'sudo tee /usr/local/bin/dmamap.bt >/dev/null' <"$V/src/dmamap.bt"; done
vcase Z1-final-sanity-tso-on 6901 "" "-S 4m -R 64" d:13:999983:0:0:4099
echo "===== CASE Z2-dma-map-recount $(date -u +%FT%TZ)"
g2 "rm -f /tmp/vrx.log; nohup python3 /usr/local/bin/vrx.py 6902 >/tmp/vrx.log 2>&1 & sleep 1"
g1 "sudo rm -f /tmp/dmamap.out; sudo nohup bpftrace /usr/local/bin/dmamap.bt >/tmp/dmamap.out 2>&1 & for i in \$(seq 300); do grep -q -E 'Attach(ed|ing)' /tmp/dmamap.out 2>/dev/null && break; sleep 0.2; done; sleep 1
    sudo vtx -e - -S 4m -R 64 rocm-ernic0 $IP2 6902 d:7:999983:4096:61:4099 | sha256sum; sleep 1; sudo pkill -INT -x bpftrace; sleep 3; sudo cat /tmp/dmamap.out"
g2 "sleep 1; cat /tmp/vrx.log"
for n in 1 2; do "$G" $n "echo guest $n: \$(uname -r) mtu \$(cat /sys/class/net/$IF/mtu) \$(ethtool -k $IF | grep -E '^(tx-checksumming|scatter-gather|tcp-segmentation-offload|generic-segmentation-offload):' | tr -s ' \n' ' ') rx-ring \$(ethtool -g $IF | awk '/^RX:/{n++; if(n==2)print \$2}') tcp_retries2=\$(cat /proc/sys/net/ipv4/tcp_retries2) xps=\$(cat /sys/class/net/$IF/queues/tx-*/xps_cpus | tr '\n' ' ') addrs=\$(ip -4 -br addr show $IF | tr -s ' ') tainted=\$(cat /proc/sys/kernel/tainted) dmabufs=\$(sudo awk '/^Total/{print \$2}' /sys/kernel/debug/dma_buf/bufinfo); ip route show | grep -c blackhole; pgrep -x 'vtx|bpftrace|tcpdump' | wc -l"; done
"$S"/stack.sh status 2>&1 | tail -4
echo "DONE z"
