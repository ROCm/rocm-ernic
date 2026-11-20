#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# After editing FILE (path below the ionic directory) in the linux-rx tree:
# copy it into stages sFROM..sLAST, provided those stages all held the same
# version of it (i.e. no later patch touches the file).
# usage: propagate.sh FILE FROM LAST
set -euo pipefail
R=${DEVMEM_ROOT:?set it to the work area}
D=drivers/net/ethernet/pensando/ionic
f=$1; from=$2; last=$3
for n in $(seq $((from + 1)) "$last"); do
	cmp -s "$R/patches-rx/stages/s$from/$D/$f" "$R/patches-rx/stages/s$n/$D/$f" ||
		{ echo "s$from and s$n differ in $f: replay by hand" >&2; exit 1; }
done
for n in $(seq "$from" "$last"); do
	cp "$R/linux-rx/$D/$f" "$R/patches-rx/stages/s$n/$D/$f"
done
echo "propagated $f to s$from..s$last"
