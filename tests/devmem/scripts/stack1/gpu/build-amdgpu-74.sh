#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Build the patched copy of v7.4-rc3's amdgpu (test/gpu/amdgpu-74, = in-tree
# drivers/gpu/drm/amd + test/gpu/patches) as an external module against the
# 7.4-rc3-p2p kernel build, in the devmem-kbuild container.
# Result: test/gpu/amdgpu-74/drivers/gpu/drm/amd/amdgpu/amdgpu.ko
set -euo pipefail
T=${DEVMEM_ROOT:?set it to the work area}/test
docker rm devmem-kbuild-amdgpu74 >/dev/null 2>&1 || true
docker run -d --name devmem-kbuild-amdgpu74 --cgroup-parent=system.slice \
    -u "$(id -u):$(id -g)" -v "$T:$T" -w "$T" devmem-kbuild:1 bash -c "
    set -e; export HOME=/tmp KBUILD_BUILD_USER=devmem KBUILD_BUILD_HOST=testbed
    make -C $T/kbuild/7.4-rc3/src O=$T/kbuild/7.4-rc3-p2p/out -j48 \
        M=$T/gpu/amdgpu-74/drivers/gpu/drm/amd/amdgpu modules
    echo '=== BUILD DONE amdgpu-74 ==='"
echo "follow: docker logs -f devmem-kbuild-amdgpu74"
