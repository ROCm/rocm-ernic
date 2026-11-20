#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Build the "-p2p" variant of a test kernel (CONFIG_PCI_P2PDMA=y) in the
# devmem-kbuild container.   kbuild-p2p.sh 7.2.15|7.4-rc3
# Container devmem-kbuild-p2p-<flavour>; debs in test/kbuild/<flavour>-p2p/.
set -euo pipefail
FLAVOUR=${1:?usage: kbuild-p2p.sh 7.2.15|7.4-rc3}
T=${DEVMEM_ROOT:?set it to the work area}/test
NAME=devmem-kbuild-p2p-$(echo "$FLAVOUR" | tr -c 'a-zA-Z0-9\n' '-')
docker rm "$NAME" >/dev/null 2>&1 || true
docker run -d --name "$NAME" --cgroup-parent=system.slice \
    -u "$(id -u):$(id -g)" -e JOBS="${JOBS:-48}" \
    -v "$T:$T" -w "$T" devmem-kbuild:1 \
    bash "$T/scripts/gpu/kbuild-p2p-in-container.sh" "$FLAVOUR"
echo "follow: docker logs -f $NAME"
