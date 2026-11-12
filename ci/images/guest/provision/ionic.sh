# shellcheck shell=bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# provision/ionic.sh
#
# Provisions the guest for the ionic RDMA driver, inside it, in a boot of
# its own after cloud-init and the mainline kernel. build.sh runs it over
# SSH as the guest's user, after a preamble that sets
# `set -euo pipefail`, RELEASE and RDMA_CORE_VERSION, so any failing
# command fails the build. Root comes from sudo.
#
# Nothing is built here. The release's rdma-core already has the ionic
# provider, and the lanes build the ionic modules themselves.

echo "=== ionic guest provisioning ==="
echo "rdma-core: ${RDMA_CORE_VERSION}"

if [[ -z ${RDMA_CORE_VERSION} ]]; then
    echo "Error: RDMA_CORE_VERSION is empty" >&2
    exit 1
fi

# The pin describes the release, so check the release agrees before
# recording it: a release that moves its rdma-core fails the build here,
# rather than publishing a guest whose stamp disagrees with its libraries
INSTALLED=$(dpkg-query -W -f='${Version}' rdma-core)
if [[ ${INSTALLED%%-*} != "${RDMA_CORE_VERSION}" ]]; then
    echo "Error: ${RELEASE} packages rdma-core ${INSTALLED}," \
        "expected ${RDMA_CORE_VERSION}" >&2
    exit 1
fi

# The userspace provider, named for its ABI (libionic-rdmav59.so on 61.0)
ls /usr/lib/*/libibverbs/libionic*.so

# The PCI ID database, newer than the archive's, which predates the
# device's ID. The lanes check the device shows up with its pci.ids names,
# so a failure here fails the build, where its cause is still visible.
sudo update-pciids

# RDMA pins memory, past the default 8 MiB RLIMIT_MEMLOCK, and ibv_reg_mr()
# fails with "Cannot allocate memory" beyond it: an unprivileged
# ib_send_bw over about 4 MiB dies with "Couldn't create CQ". Root isn't
# affected (CAP_IPC_LOCK), and `ulimit -l` under sudo still reads 8192,
# so the limit is easy to rule out wrongly.
#
# limits.d is PAM's, so it covers SSH logins and what they start; a
# systemd unit doesn't go through PAM and takes DefaultLimitMEMLOCK. Both
# take effect at the next login and boot, which the checks get.
sudo install -d -m 0755 /etc/security/limits.d
sudo tee /etc/security/limits.d/99-rdma-memlock.conf >/dev/null <<'EOF'
# Unlimited locked memory, for RDMA
*       soft    memlock     unlimited
*       hard    memlock     unlimited
root    soft    memlock     unlimited
root    hard    memlock     unlimited
EOF
sudo chmod 0644 /etc/security/limits.d/99-rdma-memlock.conf
sudo install -d -m 0755 /etc/systemd/system.conf.d
sudo tee /etc/systemd/system.conf.d/99-rdma-memlock.conf >/dev/null <<'EOF'
[Manager]
DefaultLimitMEMLOCK=infinity
EOF
sudo chmod 0644 /etc/systemd/system.conf.d/99-rdma-memlock.conf

# The stamp ernic_guest_setup reads to skip building rdma-core:
# VERSION:none, "none" for no GDA patches, newline included, as the role
# compares it with what it computes. A guest with GPU passthrough, which
# needs the patches, doesn't match, and builds.
sudo install -d -m 0755 /usr/local/share/rocm-ernic
printf '%s:none\n' "${RDMA_CORE_VERSION}" |
    sudo tee /usr/local/share/rocm-ernic/provider.stamp >/dev/null
sudo chmod 0644 /usr/local/share/rocm-ernic/provider.stamp

# Keep ionic out of every initramfs. The lanes install a patched ionic
# with DKMS; with the stock ionic.ko in the initramfs, it loads before the
# root filesystem's, and ionic_rdma, built against the patched one, then
# fails to load on a symbol version mismatch that names neither. The root
# disk is virtio and the device isn't a boot device, so nothing at boot
# needs them. A hook, so it covers every later update-initramfs,
# including those the lanes' DKMS installs run; checks/ionic.sh checks
# the finished image.
sudo install -d -m 0755 /etc/initramfs-tools/hooks
sudo tee /etc/initramfs-tools/hooks/ionic-exclude >/dev/null <<'HOOK'
#!/bin/sh
PREREQ=""
prereqs() { echo "${PREREQ}"; }
case "${1}" in prereqs) prereqs; exit 0 ;; esac
[ -n "${DESTDIR}" ] || exit 0
# When a hook runs, the modules still to be copied are listed by name in
# mkinitramfs' __MODULES_TO_ADD, and copied after the hooks, so drop them
# there; then delete any already copied.
if [ -n "${__MODULES_TO_ADD:-}" ] && [ -f "${__MODULES_TO_ADD}" ]; then
    grep -vxE 'ionic|ionic_rdma|ernic_p4' "${__MODULES_TO_ADD}" \
        > "${__MODULES_TO_ADD}.keep" || true
    mv "${__MODULES_TO_ADD}.keep" "${__MODULES_TO_ADD}"
fi
find "${DESTDIR}" -name 'ionic.ko*' -delete
find "${DESTDIR}" -name 'ionic_rdma.ko*' -delete
find "${DESTDIR}" -name 'ernic_p4.ko*' -delete
HOOK
sudo chmod 0755 /etc/initramfs-tools/hooks/ionic-exclude

# Load ionic_rdma at boot. ionic loads when its PCI ID appears, but
# ionic_rdma binds the auxiliary device ionic registers, which nothing
# loads it for, so without this the guest boots with no verbs device. One
# module a line: systemd-modules-load runs a modprobe for each.
sudo tee /etc/modules-load.d/rocm-ernic-ionic.conf >/dev/null <<'MODLOAD'
# ionic first: ionic_rdma binds an auxiliary device ionic registers.
ionic
ionic_rdma
MODLOAD
sudo chmod 0644 /etc/modules-load.d/rocm-ernic-ionic.conf

sudo update-initramfs -u -k all
