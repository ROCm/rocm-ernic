#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# After editing the linux-rx tree (which must have equalled stage s7 before the
# edit): apply the same change to stages sFROM..s7 (git apply in plain
# directories; fails if a hunk does not fit an earlier stage), so the fix lands
# in patch FROM. Then run patches-rx/scripts/mkpatches.sh and verify.sh.
#   fix-stage.sh FROM
set -euo pipefail
R=${DEVMEM_ROOT:?set it to the work area}
P=$R/patches-rx
from=${1:?usage: fix-stage.sh FROM}
D=drivers/net/ethernet/pensando/ionic
tmp=$(mktemp)
(cd "$P/stages" && mkdir -p .cur/$D && cp "$R/linux-rx/$D"/*.[ch] .cur/$D/ &&
    { git diff --no-index --no-color s7/$D .cur/$D || true; } |
    sed -E -e "s#(a|b)/s7/#\1/#g" -e "s#(a|b)/\.cur/#\1/#g" >"$tmp"; find .cur -type f -delete; find .cur -depth -type d -exec rmdir {} \;)
[ -s "$tmp" ] || { echo "tree equals s7: nothing to do"; exit 0; }
for n in $(seq "$from" 7); do
    (cd "$P/stages/s$n" && git apply --check "$tmp") || { echo "does not apply to s$n; replay by hand ($tmp)" >&2; exit 1; }
done
for n in $(seq "$from" 7); do (cd "$P/stages/s$n" && git apply "$tmp"); echo "applied to s$n"; done
cat "$tmp" >>"$R/test-rx/evidence/driver-fixes.diff"
