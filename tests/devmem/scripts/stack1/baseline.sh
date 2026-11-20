#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Baseline evidence for ionic devmem TX: driver identity, offload features,
# and an ncdevmem TX (client) attempt from devmem-1 to a socat sink on
# devmem-2. Prints everything; redirect to test/evidence/.
#
#   baseline.sh [LABEL]
set -euo pipefail
S=$(dirname "$(readlink -f "$0")")
IF=rocm-ernic0 IP2=192.168.100.12 PORT=5302
echo "# baseline ${1:-} $(date -u +%FT%TZ)"
for n in 1 2; do
    echo "===== devmem-$n ====="
    "$S/gssh" $n - <<EOF
set -u
mountpoint -q /mnt/hostfs || sudo mount /mnt/hostfs
sudo install -m 0755 /mnt/hostfs/test/bin/ncdevmem /usr/local/bin/ncdevmem
echo "\$ uname -r"; uname -r
echo "\$ modinfo ionic (filename, srcversion, vermagic)"
modinfo ionic | grep -E '^(filename|srcversion|vermagic):'
echo "module taint flags (O = out-of-tree): '\$(cat /sys/module/ionic/taint)'"
echo "\$ ethtool -i $IF"; ethtool -i $IF
echo "\$ ethtool -k $IF"; ethtool -k $IF
echo "\$ ethtool -l $IF"; ethtool -l $IF
echo "\$ ls -l /dev/udmabuf"; ls -l /dev/udmabuf
echo "\$ grep config"
grep -E '^CONFIG_(UDMABUF|NET_DEVMEM|PAGE_POOL|DEBUG_NET|DMA_API_DEBUG|KASAN|PROVE_LOCKING|DEBUG_ATOMIC_SLEEP|DEBUG_INFO_BTF)=' /boot/config-\$(uname -r)
echo "\$ sha256sum /usr/local/bin/ncdevmem"; sha256sum /usr/local/bin/ncdevmem
EOF
done
echo "===== ncdevmem TX: devmem-1 -> devmem-2 ====="
"$S/gssh" 2 "rm -f /tmp/devmem-rx.bin; nohup sh -c 'socat -u TCP-LISTEN:$PORT,bind=$IP2,reuseaddr OPEN:/tmp/devmem-rx.bin,creat,trunc' >/dev/null 2>&1 &
    for i in \$(seq 100); do ss -ltnH 'sport = :$PORT' | grep -q . && exit 0; sleep 0.1; done; echo no listener >&2; exit 1"
"$S/gssh" 1 - <<EOF
set -u
sudo dmesg -C
printf 'hello devmem tx line 1\nhello devmem tx line 2\n' > /tmp/devmem-tx.txt
echo "\$ sudo ncdevmem -f $IF -s $IP2 -p $PORT -t 0 -q 1 < /tmp/devmem-tx.txt"
sudo /usr/local/bin/ncdevmem -f $IF -s $IP2 -p $PORT -t 0 -q 1 < /tmp/devmem-tx.txt
echo "exit status: \$?"
echo "\$ sudo strace -f -e trace=sendto,sendmsg,recvmsg,recvfrom ncdevmem ... (netlink bind-tx reply)"
sudo strace -f -s 256 -e trace=sendto,sendmsg,recvmsg,recvfrom /usr/local/bin/ncdevmem -f $IF -s $IP2 -p $PORT -t 0 -q 1 < /tmp/devmem-tx.txt 2>&1 | tail -n 12
echo "\$ dmesg (since test start)"; sudo dmesg | tail -n 20
EOF
echo "===== receiver ====="
"$S/gssh" 2 "pkill -x socat; ls -l /tmp/devmem-rx.bin 2>&1; sha256sum /tmp/devmem-rx.bin 2>&1" || true
"$S/gssh" 1 "sha256sum /tmp/devmem-tx.txt"
