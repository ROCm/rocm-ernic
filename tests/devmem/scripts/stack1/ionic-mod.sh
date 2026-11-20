#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Build a patched ionic module from a patch-series directory and load it in
# the guests. Sources are staged and patched on the host (git apply, in a
# plain directory: no repository, no commit); the build is a plain
# out-of-tree `make M=` inside each guest against the running kernel's
# headers (the guests have the compiler that built their kernel).
#
#   ionic-mod.sh stage NAME BASE [SERIES_DIR...]
#       BASE = intree-7.2.15 | intree-7.4-rc3 | ernic-7.2.6
#         intree-*     the ionic Ethernet driver of that test kernel's own
#                      source (test/kbuild/<flavour>/src); builds ionic.ko
#         ernic-7.2.6  what the rocm-ernic DKMS flow builds: v7.2.6 ionic
#                      (eth + rdma + uapi) + patches/srq + patches/p4 from
#                      the rocm-ernic pin, staged as setup-ionic-dkms.sh
#                      stages it; builds ionic.ko and ionic_rdma.ko.
#                      Only for the 7.2.x kernels.
#       SERIES_DIR  directories of *.patch (kernel-root relative, -p1),
#                   applied in the order given, glob order inside each.
#                   Files with no diff (a cover letter) are skipped.
#       Result: test/ionic-mod/NAME/ (visible in guests under /mnt/hostfs).
#   ionic-mod.sh build NAME [GUESTS]    copy to ~/ionic-mod/NAME in each guest, make
#   ionic-mod.sh load NAME [GUESTS]     swap the running ionic for the built one
#   ionic-mod.sh restore [GUESTS]       back to the kernel's installed ionic (modprobe)
#   ionic-mod.sh all NAME BASE [SERIES_DIR...]   stage + build + load on both guests
#
# GUESTS defaults to "1 2". Loading is not persistent: a reboot returns to
# the installed module.
set -euo pipefail
S=$(dirname "$(readlink -f "$0")")
T=${DEVMEM_ROOT:?set it to the work area}/test
PIN=${DEVMEM_ROOT:?set it to the work area}/rocm-ernic-pin/ansible/files/patches
ETH=drivers/net/ethernet/pensando/ionic
RDMA=drivers/infiniband/hw/ionic
DOC=Documentation/networking/device_drivers/ethernet/pensando

apply_dir() { # TREE DIR
    local p
    for p in "$2"/*.patch; do
        [ -e "$p" ] || { echo "no *.patch in $2" >&2; return 1; }
        if ! grep -q '^diff --git ' "$p"; then
            echo "  skip (no diff): $(basename "$p")"
            continue
        fi
        # Only the files staged here can be patched: a patch touching
        # anything else (net core, ...) fails here, loudly.
        (cd "$1" && git apply --whitespace=nowarn -p1 "$p") ||
            { echo "FAILED to apply $p" >&2; return 1; }
        echo "  applied: $(basename "$p")"
    done
}

stage() {
    local name=$1 base=$2 d
    shift 2
    d=$T/ionic-mod/$name
    [ -e "$d" ] && { echo "$d exists; pick a new NAME" >&2; exit 1; }
    mkdir -p "$d/tree/$ETH"
    case "$base" in
    intree-7.2.15 | intree-7.4-rc3)
        cp -r "$T/kbuild/${base#intree-}/src/$ETH/." "$d/tree/$ETH/"
        # A mainline series also patches the driver's documentation; stage it
        # so the patch applies (it is not part of the module build).
        mkdir -p "$d/tree/$DOC"
        cp "$T/kbuild/${base#intree-}/src/$DOC/ionic.rst" "$d/tree/$DOC/"
        ;;
    ernic-7.2.6)
        mkdir -p "$d/tree/$RDMA" "$d/tree/include/uapi/rdma"
        cp -r "${DEVMEM_ROOT:?set it to the work area}/linux-7.2.6/$ETH/." "$d/tree/$ETH/"
        cp -r "${DEVMEM_ROOT:?set it to the work area}/linux-7.2.6/$RDMA/." "$d/tree/$RDMA/"
        cp -r "${DEVMEM_ROOT:?set it to the work area}/linux-7.2.6/include/uapi/rdma/." "$d/tree/include/uapi/rdma/"
        # The order the ERNIC bundle and the guest image use: srq, then p4.
        apply_dir "$d/tree" "$PIN/srq"
        apply_dir "$d/tree" "$PIN/p4"
        ;;
    *)
        echo "unknown BASE $base" >&2
        exit 2
        ;;
    esac
    for s in "$@"; do
        echo "series: $s"
        apply_dir "$d/tree" "$(readlink -f "$s")"
    done
    if [ "$base" = ernic-7.2.6 ]; then
        # The layout and Kbuild setup-ionic-dkms.sh writes.
        mkdir -p "$d/mod/eth" "$d/mod/rdma" "$d/mod/uapi/rdma"
        cp -r "$d/tree/$ETH/." "$d/mod/eth/"
        cp -r "$d/tree/$RDMA/." "$d/mod/rdma/"
        cp "$d/tree/include/uapi/rdma/ionic-abi.h" "$d/mod/uapi/rdma/"
        cat >"$d/mod/Kbuild" <<'EOF'
# Same as the Kbuild rocm-ernic's setup-ionic-dkms.sh generates.
ifneq ($(wildcard $(src)/uapi/rdma/ionic-abi.h),)
LINUXINCLUDE := -I$(src)/uapi $(LINUXINCLUDE)
endif

ccflags-y += -I$(src)/eth

obj-m += ionic.o
ionic-y := $(patsubst $(src)/eth/%.c,eth/%.o,\
             $(wildcard $(src)/eth/*.c))

ccflags-y += -I$(src)/rdma -I$(src)/eth
obj-m += ionic_rdma.o
ionic_rdma-y := $(patsubst $(src)/rdma/%.c,rdma/%.o,\
                  $(wildcard $(src)/rdma/*.c))
EOF
    else
        mkdir -p "$d/mod"
        cp -r "$d/tree/$ETH/." "$d/mod/"
    fi
    {
        echo "base: $base"
        for s in "$@"; do echo "series: $(readlink -f "$s")"; (cd "$s" && sha256sum ./*.patch); done
    } >"$d/SOURCE"
    echo "staged $d/mod"
}

build() {
    local name=$1 n
    for n in ${2:-1 2}; do
        "$S/gssh" "$n" - <<EOF
set -eu
mountpoint -q /mnt/hostfs || sudo mount /mnt/hostfs
mkdir -p ~/ionic-mod
[ -e ~/ionic-mod/$name ] && mv ~/ionic-mod/$name ~/ionic-mod/$name.old-\$(date +%s)
cp -r /mnt/hostfs/test/ionic-mod/$name/mod ~/ionic-mod/$name
make -s -C /lib/modules/\$(uname -r)/build M=\$HOME/ionic-mod/$name -j\$(nproc) modules 2>&1 | tail -n 30
ls -l ~/ionic-mod/$name/*.ko
echo "devmem-$n: built for \$(uname -r)"
EOF
    done
}

wait_netdev() { # guest
    "$S/gssh" "$1" - <<'EOF'
for i in $(seq 100); do [ -e /sys/class/net/rocm-ernic0 ] && break; sleep 0.2; done
sudo systemctl restart rocm-ernic-netcfg.service
for i in $(seq 100); do ip -4 -o addr show dev rocm-ernic0 | grep -q 192.168.100. && break; sleep 0.2; done
ip -br addr show rocm-ernic0
echo "ionic taint flags (O = out-of-tree): '$(cat /sys/module/ionic/taint)'"
sudo dmesg | grep -i -E 'ionic|BUG|WARNING|KASAN' | tail -n 6
EOF
}

load() {
    local name=$1 n
    for n in ${2:-1 2}; do
        "$S/gssh" "$n" - <<EOF
set -eu
cd ~/ionic-mod/$name
sudo dmesg -C
# rmmod, not modprobe -r: a module loaded with insmod may not be in modules.dep.
if [ -e /sys/module/ionic_rdma ]; then sudo rmmod ionic_rdma; fi
if [ -e /sys/module/ionic ]; then sudo rmmod ionic; fi
# Dependencies of the in-tree module, without loading it.
for dep in \$(modinfo -F depends ionic | tr ',' ' '); do sudo modprobe \$dep; done
sudo insmod ./ionic.ko
if [ -e ./ionic_rdma.ko ]; then
    for dep in \$(modinfo -F depends ./ionic_rdma.ko | tr ',' ' '); do [ \$dep = ionic ] || sudo modprobe \$dep; done
    sudo insmod ./ionic_rdma.ko
fi
EOF
        wait_netdev "$n"
    done
}

restore() {
    local n
    for n in ${1:-1 2}; do
        "$S/gssh" "$n" 'sudo dmesg -C; if [ -e /sys/module/ionic_rdma ]; then sudo rmmod ionic_rdma; fi; if [ -e /sys/module/ionic ]; then sudo rmmod ionic; fi; sudo modprobe ionic && { sudo modprobe ionic_rdma 2>/dev/null; true; }'
        wait_netdev "$n"
    done
}

cmd=${1:-}
shift || true
case "$cmd" in
stage) stage "$@" ;;
build) build "$@" ;;
load) load "$@" ;;
restore) restore "$@" ;;
all)
    stage "$@"
    build "$1"
    load "$1"
    ;;
*)
    sed -n '2,28p' "$0"
    exit 2
    ;;
esac
