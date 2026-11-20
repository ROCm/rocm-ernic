#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Build a guest kernel in the devmem-kbuild container (the host has no bison,
# and the container's gcc matches the guests').
#
#   scripts/kbuild.sh 7.2.15|7.4-rc3      # detached; follow with docker logs
#
# The container is named devmem-kbuild-<flavour>. Output: debs in
# /var/tmp/ionic-devmem/test/kbuild/<flavour>/, build tree in .../out.
set -euo pipefail
FLAVOUR=${1:?usage: kbuild.sh 7.2.15|7.4-rc3}
T=${DEVMEM_ROOT:?set it to the work area}/test
NAME=devmem-kbuild-$(echo "$FLAVOUR" | tr -c 'a-zA-Z0-9\n' '-')
docker rm "$NAME" >/dev/null 2>&1 || true
# --cgroup-parent: this host's dockerd refuses a container without one.
docker run -d --name "$NAME" --cgroup-parent=system.slice \
    -u "$(id -u):$(id -g)" -e JOBS="${JOBS:-56}" \
    -v "$T:$T" -w "$T" devmem-kbuild:1 \
    bash "$T/scripts/kbuild-in-container.sh" "$FLAVOUR"
echo "follow: docker logs -f $NAME"
