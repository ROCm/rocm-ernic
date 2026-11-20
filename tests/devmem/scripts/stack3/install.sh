#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# install.sh [GUESTS]  -- copy the end-to-end tools into each guest's ~/e2e
# and build them there.  GUESTS defaults to "1 2".
#
# devmem_rx_gpu is built with the flags every guest-side program of this
# project uses, and needs src/ernic_gnu.c and src/parse_int.h, which
# deploy.sh puts beside it in the work area.  hip_devmem_rx needs hipcc and
# the HSA runtime, so it is built in the guest and nowhere else.
set -euo pipefail
HERE=$(dirname "$(readlink -f "$0")")
# shellcheck source=tests/devmem/scripts/stack3/env.sh
. "$HERE/env.sh"

for n in ${1:-1 2}; do
    g "$n" 'mkdir -p ~/e2e'
    gscp "$n" \
        "$E2E/src/devmem_rx_gpu.c" "$E2E/src/hip_devmem_rx.cpp" \
        "$E2E/src/ernic_gnu.c" "$E2E/src/ernic_gnu.h" "$E2E/src/parse_int.h" \
        "$E2E/probes/rx-probe.bt" e2e/
    g "$n" - <<'EOS'
set -eu
cd ~/e2e
gcc -std=c11 -pedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -O2 -Wall -Wextra -static -pthread -I. \
    -o devmem_rx_gpu devmem_rx_gpu.c ernic_gnu.c
R=/opt/rocm/core-10.2
"$R/bin/hipcc" -O2 --offload-arch=gfx1250 -I"$R/include" \
    -o hip_devmem_rx hip_devmem_rx.cpp -L"$R/lib" -lhsa-runtime64 \
    -Wl,-rpath,"$R/lib"
echo "$(hostname) $(uname -r): built"
sha256sum devmem_rx_gpu hip_devmem_rx
EOS
done
