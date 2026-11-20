#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Build amdgpu-dkms 31.70 (7.2.3) against one of the test-bed kernel build
# trees, in the devmem-kbuild container (the compiler the kernels were built
# with).  The DKMS pre-build step edits its source tree in place, so each
# build works on a fresh copy.
#
#   build.sh LABEL KERNELVER HEADERS_DIR [PATCH...]
#
# HEADERS_DIR is an unpacked linux-headers package (what a guest has), which
# kbuild must be able to write temporary files into.
#
# Result: /var/tmp/ionic-devmem/amdgpu-31.70/build/LABEL/{src,make.log}
set -euo pipefail
LABEL=${1:?label} KVER=${2:?kernelver} HDR=${3:?unpacked linux-headers dir}
R=${DEVMEM_ROOT:?set it to the work area}
A=$R/amdgpu-31.70
PRISTINE=$A/x/usr/src/amdgpu-7.2.3-2428518.26.04
D=$A/build/$LABEL
[ -e "$D" ] && { echo "$D exists; pick a new label" >&2; exit 1; }
mkdir -p "$D"
cp -a "$PRISTINE" "$D/src"
for p in "${@:4}"; do
    echo "applying $p"
    patch -d "$D/src" -p1 --no-backup-if-mismatch <"$p"
done

name=amdgpu3170-$LABEL
docker rm "$name" >/dev/null 2>&1 || true
id=$(docker create --name "$name" --cgroup-parent=system.slice \
    -u "$(id -u):$(id -g)" \
    -v "$R:$R" \
    -v "$HDR:/usr/src/linux-headers-$KVER" \
    -v "$HDR:/lib/modules/$KVER/build" \
    -w "$D/src" devmem-kbuild:1 bash -c "
        export HOME=/tmp KBUILD_BUILD_USER=devmem KBUILD_BUILD_HOST=testbed
        # -k: keep going, so one log shows every error rather than the first
        export MAKEFLAGS=-k
        make KERNELVER=$KVER 2>&1
        echo \"=== make rc=\$? ===\"
        find . -name '*.ko' | sort")
docker start "$id" >/dev/null
rc=$(docker wait "$id")
docker logs "$id" >"$D/make.log" 2>&1
docker rm "$id" >/dev/null
echo "container rc=$rc; log $D/make.log"
grep -c ' error: ' "$D/make.log" | sed 's/^/compile errors: /'
tail -15 "$D/make.log"
