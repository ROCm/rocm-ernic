#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Record the linux-rx working tree's ionic files as stage sN.
# usage: snap.sh N
set -euo pipefail
R=${DEVMEM_ROOT:?set it to the work area}
L=$R/linux-rx
S=$R/patches-rx/stages/s$1
D=drivers/net/ethernet/pensando/ionic
DOC=Documentation/networking/device_drivers/ethernet/pensando
mkdir -p "$S/$D" "$S/$DOC"
cp "$L/$D"/*.c "$L/$D"/*.h "$S/$D/"
cp "$L/$DOC/ionic.rst" "$S/$DOC/"
echo "snapped s$1"
