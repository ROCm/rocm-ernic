#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Apply the ERNIC bundle's kernel patches (kernel-tools
# patches/dmabuf-72x-backport, the set build_ernic_bundle.py applies with
# `patch -p1`) to a scratch copy of v7.2.15 with `git apply`. Never commits.
set -euo pipefail
SRC=${1:?usage: apply-bundle-patches.sh SRC_DIR PATCH_DIR}
PDIR=${2:?usage: apply-bundle-patches.sh SRC_DIR PATCH_DIR}
sentinel="$SRC/.patches-applied-$(basename "$PDIR")"
if [ -e "$sentinel" ]; then
    echo "already applied: $sentinel"
    exit 0
fi
cd "$SRC"
n=0
for p in "$PDIR"/*.patch; do
    # The bundle applies with `patch -p1`, which tolerates fuzz; 0019 needs
    # it (fuzz 2 in io_uring/rw.c on v7.2.15). -C1 is git apply's equivalent.
    if git apply --whitespace=nowarn "$p" 2>/dev/null; then
        echo "applied $(basename "$p")"
    else
        git apply --whitespace=nowarn -C1 "$p"
        echo "applied with reduced context (-C1): $(basename "$p")"
    fi
    n=$((n + 1))
done
touch "$sentinel"
echo "applied $n patches"
