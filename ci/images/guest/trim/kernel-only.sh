# shellcheck shell=bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# trim/kernel-only.sh
#
# Removes every kernel but the one the guest boots, inside it, in a boot
# of its own after provisioning and before the verify boot, so the checks
# see what ships. build.sh runs it over SSH as the guest's user, after a
# preamble that sets `set -euo pipefail`. Root comes from sudo.
#
# Installing the mainline kernel leaves the cloud image's own installed
# beside it, with grub pinned to the mainline one. With one kernel there's
# nothing to boot by mistake, and the disk is about 370 MB smaller: the release
# kernel's modules, headers, image and initramfs. The compiler, headers
# and DKMS stay: the lanes build the ionic modules in the guest.

KVER=$(uname -r)
# The ABI without the flavour: the shared headers package is
# linux-headers-7.2.6-070206, with no -generic, and /lib/modules/KVER/build
# points into it, so matching the whole of `uname -r` would purge it
KABI=${KVER%-generic}

echo "kernel: ${KVER}  (abi ${KABI})"

APT=(sudo DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=120)

# Every kernel package of another ABI, and the unversioned metapackages
# (linux-image-virtual, linux-headers-generic, ...), which would pull the
# release's kernel back in on the next upgrade
STALE=$(dpkg-query -W -f='${Package}\n' \
    'linux-image-*' 'linux-modules-*' 'linux-headers-*' 2>/dev/null |
    grep -v -- "${KABI}") || STALE=""

if [[ -z ${STALE} ]]; then
    echo "no kernel packages to purge; already down to ${KVER}"
else
    echo "purging kernel packages that are not ${KABI}:"
    printf '%s\n' "${STALE}" | sed 's/^/  /'
    mapfile -t STALE_PKGS <<<"${STALE}"
    "${APT[@]}" purge -y "${STALE_PKGS[@]}"
fi

"${APT[@]}" autoremove -y --purge
sudo update-grub

echo "kernels remaining:"
dpkg-query -W -f='${Package}\n' 'linux-image-*' 'linux-modules-*' \
    'linux-headers-*' 2>/dev/null | sed 's/^/  /' || true
for vmlinuz in /boot/vmlinuz-*; do
    echo "  ${vmlinuz}"
done

# The headers the lanes build against must have survived; checked here
# too, where the purge that would have removed them is still in the log
test -d "/lib/modules/${KVER}/build"
echo "headers intact: /lib/modules/${KVER}/build"

# An initramfs whose kernel has no modules tree belongs to nothing: no
# grub entry boots it and nothing regenerates it
for initrd in /boot/initrd.img-*; do
    [[ -e ${initrd} ]] || continue
    [[ -d /lib/modules/${initrd#/boot/initrd.img-} ]] && continue
    echo "removing orphaned ${initrd}"
    sudo rm -f -- "${initrd}"
done

# Give the freed blocks back to the disk image: build.sh attaches it with
# discard=unmap, then compacts it
sudo fstrim -av
