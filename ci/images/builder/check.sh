#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# check.sh
#
# Checks a freshly built builder image, from inside it, before
# ci/images/ensure-image.sh publishes it: that its default user isn't
# root, that the tools the VM lanes use are on PATH, that QEMU has the
# vfio-user client, and that libvfio-user is the pinned commit with the
# API the server needs. It fails on the first thing wrong.
#
# Usage, from ensure-image.sh:
#   docker run --rm --env-file pins.env -v check.sh:/check.sh:ro IMAGE \
#       bash /check.sh

set -euo pipefail

fail() {
    echo "check.sh: $*" >&2
    exit 1
}

: "${LIBVFIO_USER_COMMIT:?pass pins.env with --env-file}"

# Run as the image's default user, which mustn't be root; the lanes ask
# for root themselves
[[ $(id -u) -ne 0 ]] || fail "the image's default user is root"

for tool in ansible-galaxy ansible-playbook cloud-localds cmake curl file \
    gcc git ip jq meson ninja oras pkg-config python3 qemu-img \
    qemu-system-x86_64 scp setsid ss ssh ssh-keygen timeout zstd; do
    command -v "$tool" >/dev/null || fail "$tool isn't on PATH"
done

[[ $(command -v qemu-system-x86_64) == /opt/qemu/bin/* ]] ||
    fail "qemu-system-x86_64 isn't /opt/qemu's"
devices=$(qemu-system-x86_64 -device help)
[[ $devices == *'name "vfio-user-pci"'* ]] ||
    fail "QEMU has no vfio-user-pci device"

HDR=/usr/local/include/vfio-user/libvfio-user.h
[[ -f $HDR ]] || fail "$HDR is missing"
# The vfu_setup_device_dma() max_regions parameter (f3742ed9, #835),
# which the server calls. Each listing is read whole into a variable,
# not piped to grep -q, which would stop reading at the first match and
# could fail the writer, and so the check, with SIGPIPE under pipefail.
dma=$(grep -A2 'vfu_setup_device_dma' "$HDR") || dma=""
[[ $dma == *max_regions* ]] ||
    fail "libvfio-user predates vfu_setup_device_dma()'s max_regions"
built=$(cat /usr/local/share/libvfio-user-commit.txt)
[[ $built == "$LIBVFIO_USER_COMMIT" ]] ||
    fail "libvfio-user is $built, not the pinned $LIBVFIO_USER_COMMIT"
cache=$(ldconfig -p)
[[ $cache == *libvfio-user.so* ]] ||
    fail "the dynamic linker can't find libvfio-user"

for pkg in glib-2.0 json-c libibverbs librdmacm; do
    pkg-config --exists "$pkg" || fail "pkg-config can't find $pkg"
done

echo "check.sh: the image has everything the lanes use"
version=$(qemu-system-x86_64 --version)
printf '%s\n' "${version%%$'\n'*}"
echo "libvfio-user $built"
