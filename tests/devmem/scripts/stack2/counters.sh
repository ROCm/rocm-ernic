#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# counters.sh N : non-zero rx csum/hds/drop counters of guest N (totals only unless $2=all)
S=$(dirname "$(readlink -f "$0")")
"$S/gssh" "$1" "ethtool -S rocm-ernic0 | grep -E 'rx_csum_(none|complete|unnecessary|error)|rx_hds|rx_.*hds|unreadable|rx_[0-9]+_dropped|rx_dropped|alloc_err|hwstamp' | grep -v ': 0\$' | tr -s ' ' | tr '\n' ';'"; echo
