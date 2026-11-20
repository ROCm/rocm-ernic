#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Receiver guest 1: one channel, header split on.
# shellcheck source=tests/devmem/scripts/stack2/tdlib.sh
. "$(dirname "$(readlink -f "$0")")/tdlib.sh"
g 1 "sudo pkill -x ncdevmem; sudo ethtool -L rocm-ernic0 combined ${1:-1}; sudo ethtool -G rocm-ernic0 tcp-data-split on; ethtool -l rocm-ernic0 | tail -n 1; ethtool -g rocm-ernic0 | grep -i split; for i in \$(seq 50); do ping -c1 -W1 $IP2 >/dev/null 2>&1 && break; sleep 0.2; done; echo ping tries \$i; sudo dmesg -C"
g 2 'sudo dmesg -C'
