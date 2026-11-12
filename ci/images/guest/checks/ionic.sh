# shellcheck shell=bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# checks/ionic.sh
#
# Checks the finished guest, inside it, in the verify boot. build.sh
# runs it over SSH as the guest's user, after a preamble that sets
# `set -euo pipefail` and KERNEL_REF, and traces it, so the command that
# fails the build is in the log.

# The pinned mainline kernel, which pins.env keeps equal to the ionic
# sources' IONIC_KERNEL_REF, so the lanes build against matching headers.
# build.sh checks grub boots it by default.
KVER=$(uname -r)
[[ ${KVER} == "${KERNEL_REF#v}"-* ]]

# drivers/infiniband/hw/ionic arrived in 6.18. Compare whole versions:
# "major >= 6 and minor >= 18" rejects 7.0.
[[ $(printf '6.18\n%s\n' "${KVER}" | sort -V) == 6.18$'\n'* ]]

# Headers matching the kernel, which DKMS builds against, with
# ib_umem_get_va, which the ionic sources call and which arrived after 7.0
test -d "/lib/modules/${KVER}/build"
grep -q ib_umem_get_va "/lib/modules/${KVER}/build/include/rdma/ib_umem.h"

# The toolchain and rdma-core's build dependencies, which the lanes don't
# install because they're here
command -v gcc make cmake ninja dkms git
python3 -c 'import docutils'
pkg-config --exists libnl-3.0 libnl-route-3.0 libudev libsystemd

# Verbs userspace, and its ionic provider
command -v ibv_devinfo
ls /usr/lib/*/libibverbs/libionic*.so

# The kernel's own ionic and ionic_rdma: the lanes build theirs with
# DKMS, so none may be here already
for module in ionic ionic_rdma; do
    path=$(modinfo -n "${module}")
    case ${path} in
        */updates/dkms/*)
            echo "ERROR: ${module} is ${path}, a DKMS module" >&2
            exit 1
            ;;
        "")
            echo "ERROR: no module found for ${module}" >&2
            exit 1
            ;;
        *) echo "  ${module}: ${path}" ;;
    esac
done

# ionic_rdma loads at boot, after ionic
conf=/etc/modules-load.d/rocm-ernic-ionic.conf
[[ $(grep -vE '^[[:space:]]*(#|$)' "${conf}") == $'ionic\nionic_rdma' ]]

# No ionic module in any initramfs: one there loads before the lanes'
# DKMS build, and ionic_rdma then fails on a symbol version mismatch
for initrd in /boot/initrd.img-*; do
    [[ -e ${initrd} ]] || continue
    listing=$(lsinitramfs "${initrd}")
    if grep -qE '(^|/)(ionic|ionic_rdma|ernic_p4)\.ko(\.[a-z]+)?$' \
        <<<"${listing}"; then
        echo "ERROR: ${initrd} holds an ionic module" >&2
        grep -E 'ionic|ernic_p4' <<<"${listing}" >&2 || true
        exit 1
    fi
done

# The memlock limit provision/ionic.sh raised, in this new login
[[ $(ulimit -l) == unlimited ]]

# The stamp ernic_guest_setup reads
[[ -s /usr/local/share/rocm-ernic/provider.stamp ]]
