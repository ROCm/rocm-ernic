#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Stack 2: build ionic.ko + ionic_rdma.ko from a kernel tree's CURRENT working
# state (default /var/tmp/ionic-devmem/linux-rx = v7.4-rc3 + TX + RX series)
# and load them in the rx guests.
#
#   ionic-mod.sh stage NAME [TREE]     snapshot eth + rdma + uapi into test-rx/ionic-mod/NAME
#   ionic-mod.sh build NAME [GUESTS]   copy into each guest, make M=
#   ionic-mod.sh load NAME [GUESTS]    rmmod ionic_rdma, ionic; insmod the built pair
#   ionic-mod.sh unload [GUESTS]
#   ionic-mod.sh all NAME [TREE]
set -euo pipefail
S=$(dirname "$(readlink -f "$0")")
T=${DEVMEM_ROOT:?set it to the work area}/test-rx
ETH=drivers/net/ethernet/pensando/ionic
RDMA=drivers/infiniband/hw/ionic

stage() {
    local name=$1 tree=${2:-${DEVMEM_ROOT:?set it to the work area}/linux-rx} d
    d=$T/ionic-mod/$name
    [ -e "$d" ] && { echo "$d exists; pick a new NAME" >&2; exit 1; }
    mkdir -p "$d/mod/eth" "$d/mod/rdma" "$d/mod/uapi/rdma"
    cp -r "$tree/$ETH/." "$d/mod/eth/"
    cp -r "$tree/$RDMA/." "$d/mod/rdma/"
    cp "$tree/include/uapi/rdma/ionic-abi.h" "$d/mod/uapi/rdma/"
    find "$d/mod" \( -name '*.o' -o -name '*.cmd' -o -name '*.ko' -o -name '*.mod*' \) -delete
    cat >"$d/mod/Kbuild" <<'KB'
LINUXINCLUDE := -I$(src)/uapi $(LINUXINCLUDE)
ccflags-y += -I$(src)/eth
obj-m += ionic.o
ionic-y := $(patsubst $(src)/eth/%.c,eth/%.o,$(wildcard $(src)/eth/*.c))
ccflags-y += -I$(src)/rdma
obj-m += ionic_rdma.o
ionic_rdma-y := $(patsubst $(src)/rdma/%.c,rdma/%.o,$(wildcard $(src)/rdma/*.c))
KB
    {
        echo "tree: $tree"
        (cd "$tree" && GIT_OPTIONAL_LOCKS=0 git rev-parse HEAD && GIT_OPTIONAL_LOCKS=0 git diff --stat | tail -n 1)
        (cd "$d/mod" && find . -type f | sort | xargs sha256sum)
    } >"$d/SOURCE"
    echo "staged $d/mod"
}

build_one() {
    local name=$1 n=$2
    "$S/gssh" "$n" - <<EOS
set -eu
mountpoint -q /mnt/hostfs || sudo mount /mnt/hostfs
mkdir -p ~/ionic-mod
[ -e ~/ionic-mod/$name ] && mv ~/ionic-mod/$name ~/ionic-mod/$name.old-\$(date +%s)
cp -r /mnt/hostfs/test-rx/ionic-mod/$name/mod ~/ionic-mod/$name
make -s -C /lib/modules/\$(uname -r)/build M=\$HOME/ionic-mod/$name -j\$(nproc) modules 2>&1 | tail -n 30
ls -l ~/ionic-mod/$name/*.ko
echo "rx-$n: built for \$(uname -r)"
EOS
}

build() {
    local name=$1 n
    for n in ${2:-1 2}; do build_one "$name" "$n" & done
    wait
}

wait_netdev() {
    "$S/gssh" "$1" - <<'EOS'
for i in $(seq 100); do [ -e /sys/class/net/rocm-ernic0 ] && break; sleep 0.2; done
sudo systemctl restart rocm-ernic-netcfg.service
for i in $(seq 100); do ip -4 -o addr show dev rocm-ernic0 2>/dev/null | grep -q 192.168.100. && break; sleep 0.2; done
ip -br addr show rocm-ernic0
sudo dmesg | grep -i -E 'ionic|BUG|WARNING|KASAN|lockdep' | tail -n 8
EOS
}

unload() {
    local n
    for n in ${1:-1 2}; do
        "$S/gssh" "$n" 'if [ -e /sys/module/ionic_rdma ]; then sudo rmmod ionic_rdma; fi; if [ -e /sys/module/ionic ]; then sudo rmmod ionic; fi'
    done
}

load() {
    local name=$1 n
    unload "${2:-1 2}"
    for n in ${2:-1 2}; do
        "$S/gssh" "$n" - <<EOS
set -eu
cd ~/ionic-mod/$name
for dep in \$(modinfo -F depends ./ionic.ko | tr ',' ' '); do sudo modprobe \$dep; done
sudo insmod ./ionic.ko
for dep in \$(modinfo -F depends ./ionic_rdma.ko | tr ',' ' '); do [ \$dep = ionic ] || sudo modprobe \$dep; done
sudo insmod ./ionic_rdma.ko
EOS
        wait_netdev "$n"
    done
}

cmd=${1:-}
shift || true
case "$cmd" in
stage) stage "$@" ;;
build) build "$@" ;;
load) load "$@" ;;
unload) unload "$@" ;;
all)
    stage "$@"
    build "$1"
    load "$1"
    ;;
*)
    sed -n '2,12p' "$0"
    exit 2
    ;;
esac
