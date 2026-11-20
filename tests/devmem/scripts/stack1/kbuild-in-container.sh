#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Runs INSIDE the devmem-kbuild container (see scripts/kbuild.sh).
# Configure and build a guest kernel out-of-tree and package it as debs.
#
#   kbuild-in-container.sh <flavour>     flavour: 7.2.15 | 7.4-rc3
#
# Config = make defconfig + kernel-tools config-ernic-bundle (which pulls in
# config-default, config-amdgpu-dkms, config-infiniband) + the devmem/debug
# options below + olddefconfig.
set -euo pipefail
FLAVOUR=${1:?flavour}
T=${DEVMEM_ROOT:?set it to the work area}/test
SRC=$T/kbuild/$FLAVOUR/src
OUT=$T/kbuild/$FLAVOUR/out
CS=$T/kbuild/config-scripts
JOBS=${JOBS:-56}
export HOME=/tmp KBUILD_BUILD_USER=devmem KBUILD_BUILD_HOST=testbed

case "$FLAVOUR" in
7.2.15) LOCALVERSION=-devmem ;;
7.4-rc3) LOCALVERSION=-devmem-kasan ;;
*) echo "unknown flavour $FLAVOUR" >&2; exit 2 ;;
esac

mkdir -p "$OUT"
cd "$SRC"
if [ ! -e "$OUT/.config" ]; then
    make O="$OUT" defconfig
    # The config scripts call ./scripts/config, which edits ./.config, so run
    # them from the output directory with the source's scripts/ linked in.
    cd "$OUT"
    cfg() { "$SRC/scripts/config" --file "$OUT/.config" "$@"; }
    # config-ernic-bundle uses "./scripts/config": give it one that targets
    # this .config.
    mkdir -p "$OUT/.cfgshim/scripts"
    printf '#!/bin/sh\nexec %s/scripts/config --file %s/.config "$@"\n' \
        "$SRC" "$OUT" >"$OUT/.cfgshim/scripts/config"
    chmod +x "$OUT/.cfgshim/scripts/config"
    (cd "$OUT/.cfgshim" && SCRIPT_DIR="$CS" bash "$CS/config-ernic-bundle")

    # ---- devmem TX test bed additions (both flavours) ----
    cfg -e MEMFD_CREATE -e SHMEM -e TMPFS -e SYNC_FILE -e DMA_SHARED_BUFFER
    cfg -e UDMABUF -e PAGE_POOL -e PAGE_POOL_STATS -e NET_DEVMEM
    cfg -e HUGETLBFS -e IPV6
    cfg -e DEBUG_KERNEL -e DEBUG_NET -e DEBUG_FS
    cfg -e DMA_API_DEBUG -e DMA_API_DEBUG_SG -e DMABUF_DEBUG
    cfg -e FTRACE -e FUNCTION_TRACER -e FUNCTION_GRAPH_TRACER \
        -e DYNAMIC_FTRACE -e FTRACE_SYSCALLS -e KPROBES -e KPROBE_EVENTS \
        -e UPROBE_EVENTS -e FPROBE -e BPF_SYSCALL -e BPF_JIT -e BPF_EVENTS \
        -e BPF_LSM -e IKHEADERS
    # BTF needs real debug info; config-ernic-bundle chose DEBUG_INFO_NONE to
    # keep the -dbg deb away. Re-enable it and skip the -dbg deb instead
    # (DEB_BUILD_PROFILES below).
    cfg -d DEBUG_INFO_NONE -e DEBUG_INFO_DWARF5 -e DEBUG_INFO_BTF \
        -e DEBUG_INFO_BTF_MODULES -d DEBUG_INFO_REDUCED -d DEBUG_INFO_SPLIT
    cfg -m NET_VENDOR_PENSANDO -m IONIC
    # Boot from the virtio disk and mount the 9p share without help.
    cfg -e VIRTIO_PCI -e VIRTIO_BLK -e VIRTIO_NET -e EXT4_FS
    cfg -e NET_9P -e NET_9P_VIRTIO -e 9P_FS
    cfg -e MAGIC_SYSRQ

    if [ "$FLAVOUR" = 7.4-rc3 ]; then
        cfg -e KASAN -e KASAN_GENERIC -e KASAN_INLINE -e KASAN_VMALLOC
        cfg -d KFENCE
        cfg -e PROVE_LOCKING -e DEBUG_LOCKDEP -e DEBUG_ATOMIC_SLEEP \
            -e DEBUG_SPINLOCK -e DEBUG_MUTEXES -e DEBUG_LIST
    fi
    cd "$SRC"
    make O="$OUT" olddefconfig
fi

echo "=== key options ($FLAVOUR) ==="
grep -E '^(# )?CONFIG_(UDMABUF|NET_DEVMEM|PAGE_POOL|MEMFD_CREATE|DEBUG_NET|DMA_API_DEBUG|FTRACE|KPROBES|BPF_SYSCALL|DEBUG_INFO_BTF|KASAN|PROVE_LOCKING|DEBUG_ATOMIC_SLEEP|KFENCE|IONIC|INFINIBAND|NET_9P_VIRTIO|9P_FS|VIRTIO_BLK|LOCALVERSION)[ =]' "$OUT/.config" || true

# pkg.linux-upstream.nokerneldbg: do not build the multi-GB -dbg deb.
make O="$OUT" -j"$JOBS" LOCALVERSION="$LOCALVERSION" \
    DEB_BUILD_PROFILES='pkg.linux-upstream.nokerneldbg' bindeb-pkg
echo "=== BUILD DONE $FLAVOUR ==="
ls -la "$T/kbuild/$FLAVOUR"/*.deb 2>/dev/null || ls -la "$T/kbuild"/*.deb
