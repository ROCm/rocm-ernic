#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# M10: sustained send, verified continuously by the receiver.
# shellcheck source=tests/devmem/scripts/stack1/devmem-tx/lib.sh
. "$(dirname "$(readlink -f "$0")")/lib.sh"
RXWAIT=$(( ${1:-360} + 60 )) devmem_case "M10-sustained-${1:-360}s" 5490 "-S 16m -o 17 -l 1000003 -z 250001 -g 4099 -a 65521 -n 0" "-T ${1:-360} -v -C 60" "-v -W 30"
