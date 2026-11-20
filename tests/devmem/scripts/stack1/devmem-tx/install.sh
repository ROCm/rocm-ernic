#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Copy the test program into both guests (run after every rebuild).
S=${DEVMEM_ROOT:?set it to the work area}/test/scripts
for n in 1 2; do "$S/gssh" $n 'sudo sysctl -q vm.drop_caches=3; sudo install -m 0755 /mnt/hostfs/test/bin/devmem_tx /usr/local/bin/devmem_tx; sha256sum /usr/local/bin/devmem_tx'; done
sha256sum "${DEVMEM_ROOT:?set it to the work area}/test/bin/devmem_tx"
