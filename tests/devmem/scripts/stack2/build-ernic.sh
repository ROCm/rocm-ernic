#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Build a rocm-ernic server binary for the test stack WITHOUT building in (or
# writing to) the rocm-ernic checkout: take a snapshot of the sources into
# test/ernic/<label>/src and build it inside the stack's own <ernic-ref>
# image, which carries the compiler and the fork libvfio-user (vfu.3719c3a)
# the stack's QEMU was built against. A binary built on the host links the
# host's /usr/local libvfio-user instead and must not be used.
#
#   build-ernic.sh LABEL [REF]
#
#   REF given    snapshot = `git archive REF` of the checkout (committed state)
#   REF omitted  snapshot = the working tree as it is now, uncommitted and
#                untracked (not ignored) files included
#
# Result: test/ernic/LABEL/build/rocm-ernic; then
#   scripts/stack.sh server /var/tmp/ionic-devmem/test/ernic/LABEL/build/rocm-ernic
set -euo pipefail
LABEL=${1:?usage: build-ernic.sh LABEL [REF]}
REF=${2:-}
REPO=${ERNIC_REPO:-$HOME/Projects/rocm-ernic}
T=${DEVMEM_ROOT:?set it to the work area}/test-rx
IMG=docker.io/sbates130272/batesste-ci-images-ubuntu-rocm-ernic-sbates-fork:20261112.gdca6903-ernic.90434a2-vfu.3719c3a
D=$T/ernic/$LABEL
[ -e "$D" ] && { echo "$D exists; pick a new label" >&2; exit 1; }
mkdir -p "$D/src" "$D/build"
export GIT_OPTIONAL_LOCKS=0
if [ -n "$REF" ]; then
    git -C "$REPO" archive "$REF" | tar -x -C "$D/src"
    git -C "$REPO" rev-parse "$REF" >"$D/SOURCE"
else
    git -C "$REPO" ls-files -z --cached --others --exclude-standard |
        (cd "$REPO" && tar --null -T - --ignore-failed-read -cf -) | tar -x -C "$D/src"
    {
        git -C "$REPO" rev-parse HEAD
        echo "plus working tree changes:"
        git -C "$REPO" status --short
    } >"$D/SOURCE"
fi
cat >"$D/build.sh" <<'EOF'
set -eu
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DERNIC_BUILD_P4=ON \
    -DCMAKE_PREFIX_PATH=/usr/local
cmake --build /build --target rocm-ernic
ldd /build/rocm-ernic | grep vfio
EOF
# --cgroup-parent: this host's dockerd refuses a container without one;
# attached stdout is not delivered here either, so run via drun.sh.
"$T/scripts/drun.sh" -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$D/src:/src:ro" -v "$D/build:/build" -v "$D/build.sh:/build.sh:ro" \
    --entrypoint sh "$IMG" /build.sh >"$D/build.log" 2>&1 || {
    tail -n 30 "$D/build.log" >&2
    exit 1
}
tail -n 3 "$D/build.log"
sha256sum "$D/build/rocm-ernic"
