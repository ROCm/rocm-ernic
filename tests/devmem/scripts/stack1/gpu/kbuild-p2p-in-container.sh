#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Runs INSIDE the devmem-kbuild container (see kbuild-p2p.sh).
# Build a second copy of a test kernel that differs from kbuild/<flavour>/out
# only by CONFIG_ZONE_DEVICE + CONFIG_PCI_P2PDMA (and what they need), so that
# amdgpu's pci_p2pdma_distance() check can succeed. Release gets "-p2p"
# appended, so it installs beside the original kernel.
#
#   kbuild-p2p-in-container.sh 7.2.15|7.4-rc3
set -euo pipefail
FLAVOUR=${1:?flavour}
T=${DEVMEM_ROOT:?set it to the work area}/test
SRC=$T/kbuild/$FLAVOUR/src
BASE=$T/kbuild/$FLAVOUR/out
OUT=$T/kbuild/$FLAVOUR-p2p/out
JOBS=${JOBS:-48}
export HOME=/tmp KBUILD_BUILD_USER=devmem KBUILD_BUILD_HOST=testbed
case "$FLAVOUR" in
7.2.15) LOCALVERSION=-devmem-p2p ;;
7.4-rc3) LOCALVERSION=-devmem-kasan-p2p ;;
*) echo "unknown flavour $FLAVOUR" >&2; exit 2 ;;
esac
mkdir -p "$OUT"
cd "$SRC"
if [ ! -e "$OUT/.config" ]; then
    cp "$BASE/.config" "$OUT/.config"
    cfg() { "$SRC/scripts/config" --file "$OUT/.config" "$@"; }
    cfg -e MEMORY_HOTPLUG -e MEMORY_HOTREMOVE -e ZONE_DEVICE -e PCI_P2PDMA
    make O="$OUT" olddefconfig
fi
echo "=== key options ($FLAVOUR-p2p) ==="
grep -E '^(# )?CONFIG_(ZONE_DEVICE|PCI_P2PDMA|MEMORY_HOTPLUG|MEMORY_HOTREMOVE|DRM_AMDGPU|NET_DEVMEM|UDMABUF|KASAN|IONIC)[ =]' "$OUT/.config" || true
echo "=== config diff against $BASE ==="
diff <(grep -v '^#' "$BASE/.config" | sort) <(grep -v '^#' "$OUT/.config" | sort) || true
make O="$OUT" -j"$JOBS" LOCALVERSION="$LOCALVERSION" \
    DEB_BUILD_PROFILES='pkg.linux-upstream.nokerneldbg' bindeb-pkg
echo "=== BUILD DONE $FLAVOUR-p2p ==="
ls -la "$T/kbuild/$FLAVOUR-p2p"/*.deb
