#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Build a static ncdevmem from the v7.4-rc3 selftests without touching any
# kernel checkout: export the needed subset of the tag into test/ncdevmem/src
# and build there. Result: test/bin/ncdevmem (visible to the guests at
# /mnt/hostfs/test/bin/ncdevmem).
set -euo pipefail
T=${DEVMEM_ROOT:?set it to the work area}/test
S=$T/ncdevmem/src
mkdir -p "$S" "$T/bin"
if [ ! -e "$S/MAINTAINERS" ]; then
    # MAINTAINERS is mandatory: ynl_gen_c.py's find_kernel_root() loops
    # forever without it.
    GIT_OPTIONAL_LOCKS=0 git -C "${DEVMEM_ROOT:?set it to the work area}/linux" archive v7.4-rc3 \
        MAINTAINERS Makefile Kbuild Kconfig scripts include/uapi \
        include/linux/compiler_types.h arch/x86 tools/net/ynl tools/scripts \
        tools/build tools/include tools/arch/x86 tools/testing/selftests \
        Documentation/netlink usr | tar -x -C "$S"
fi
make -C "$S" headers >/dev/null
D=$S/tools/testing/selftests/drivers/net/hw
make -C "$D" "$D/ncdevmem" LDFLAGS=-static
cp "$D/ncdevmem" "$T/bin/ncdevmem"
file "$T/bin/ncdevmem"
sha256sum "$T/bin/ncdevmem"
