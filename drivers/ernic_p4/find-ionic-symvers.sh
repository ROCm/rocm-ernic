#!/bin/bash
#
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Print the Module.symvers ernic_p4 must be built against for a given
# kernel, regenerating it when what is on disk was built for another one.
#
# ionic is out of tree here, and patches/p4 adds a field to struct
# ionic_lif, so every exported symbol taking that type gets a different
# genksyms CRC. A module built against the wrong CRCs compiles,
# installs, and then will not insert, with nothing but a dmesg line
# saying a symbol "disagrees about version" to point at the cause.
#
# "The wrong CRCs" includes the right tree built for the wrong kernel,
# which is the case that matters on an upgrade: the rebuild is for the
# NEW kernel, while anything lying around was built for the old one. A
# Module.symvers carries no record of which kernel produced it, so each
# tree gets a stamp naming one, and an unstamped or mismatched tree is
# rebuilt rather than believed.
#
# Only trees carrying a dkms.conf are considered. /usr/src holds both
# the ionic DKMS module directory and, on a guest this role provisioned,
# a whole kernel checkout under a name matching the same glob; running
# "make M=" against the latter would try to build the kernel.
#
# Absolute shebang: this is installed under /usr/src and runs as root
# from the module's Makefile under dkms, where PATH is not ours to assume.

set -euo pipefail

SYMBOL=ionic_adminq_post_wait

if [ $# -lt 1 ]; then
    echo "usage: ${0##*/} <kernelver> [ionic-dkms-source-dir ...]" >&2
    exit 2
fi

KVER="$1"
shift

TREES=()
if [ $# -gt 0 ]; then
    TREES=("$@")
else
    for d in /usr/src/ionic-ernic-*/; do
        if [ -f "${d}dkms.conf" ]; then
            TREES+=("${d%/}")
        fi
    done
fi

if [ ${#TREES[@]} -eq 0 ]; then
    echo "ERROR: no ionic DKMS source directory under /usr/src" >&2
    exit 1
fi

KDIR="/lib/modules/${KVER}/build"
if [ ! -d "${KDIR}" ]; then
    echo "ERROR: no build tree for ${KVER} at ${KDIR}" >&2
    exit 1
fi

# Why each candidate was passed over. Printed only if none works: a
# bare "found nothing" here costs a CI round trip to learn which of
# "built nothing", "built but exports nothing" and "no tree at all" it
# was, and every one of those has happened.
reasons=()

for tree in "${TREES[@]}"; do
    symvers="${tree}/Module.symvers"
    stamp="${tree}/.symvers-kernel"

    if [ ! -f "${symvers}" ] ||
        [ "$(cat "${stamp}" 2>/dev/null)" != "${KVER}" ]; then
        # dkms empties its own build directory after installing, so on a
        # provisioned guest this is the usual path, not the exception.
        rm -f "${stamp}"
        if ! log="$(make -C "${KDIR}" M="${tree}" modules 2>&1)"; then
            reasons+=("${tree}: build failed: $(printf '%s' "${log}" |
                tail -3 | tr '\n' ' ')")
            continue
        fi
        printf '%s\n' "${KVER}" >"${stamp}"
    fi

    if grep -q "${SYMBOL}" "${symvers}" 2>/dev/null; then
        printf '%s\n' "${symvers}"
        exit 0
    fi
    reasons+=("${tree}: built, but exports no ${SYMBOL}")
done

{
    echo "ERROR: no Module.symvers exporting ${SYMBOL} for ${KVER}"
    for r in "${reasons[@]}"; do
        echo "  ${r}"
    done
} >&2
exit 1
