#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Install a host-built guest kernel (bindeb-pkg debs under test/kbuild/<flavour>/,
# read over the 9p share) into both guests, make it the GRUB default, reboot,
# and wait for it.
#
#   install-kernel.sh 7.2.15|7.4-rc3|7.4-rc3-vlan|stock [GUESTS]     GUESTS default "1 2"
#
# "stock" only re-selects the image's own 7.2.6-070206-generic.
# The image pins its boot kernel with GRUB_TOP_LEVEL in
# /etc/default/grub.d/99-rocm-ernic-kernel.cfg; zz-devmem.cfg sorts after it.
set -euo pipefail
S=$(dirname "$(readlink -f "$0")")
FL=${1:?usage: install-kernel.sh 7.2.15|7.4-rc3|7.4-rc3-vlan|stock [guests]}
GUESTS=${2:-1 2}
DEBDIR=/mnt/hostfs/test/kbuild/$FL
case "$FL" in
7.2.15) KREL=7.2.15-devmem ;;
7.4-rc3) KREL=7.4.0-rc3-devmem-kasan ;;
7.4-rc3-vlan) KREL=7.4.0-rc3-devmem-kasan-vlan DEBDIR=/mnt/hostfs/test-rx/kbuild/7.4-rc3-vlan ;;
stock) KREL=7.2.6-070206-generic ;;
*) echo "unknown flavour" >&2; exit 2 ;;
esac
one() {
    local n=$1
    "$S/gssh" "$n" - <<EOS
set -eu
mountpoint -q /mnt/hostfs || sudo mount /mnt/hostfs
if [ "$FL" != stock ] && [ ! -e /boot/vmlinuz-$KREL ]; then
    # The image's amdgpu DKMS module autoinstalls for every new kernel and
    # does not build against these (about 3 minutes, then an error). Without
    # this file that failure leaves linux-image half-configured.
    sudo touch /etc/dkms/no-autoinstall-errors
    sudo DEBIAN_FRONTEND=noninteractive dpkg -i \
        $DEBDIR/linux-headers-${KREL}_*.deb \
        $DEBDIR/linux-image-${KREL}_*.deb || echo "dpkg rc=\$? (see dkms status below)"
fi
test -e /boot/vmlinuz-$KREL
printf '# written by the devmem PoC harness, install-kernel.sh\nGRUB_TOP_LEVEL="/boot/vmlinuz-%s"\nGRUB_DEFAULT=0\n' "$KREL" |
    sudo tee /etc/default/grub.d/zz-devmem.cfg >/dev/null
sudo update-grub 2>&1 | grep -E 'Found linux|error' || true
sudo dkms status || true
# Detached, so this ssh session ends before the guest goes down (a session
# cut by the reboot can hang: SLIRP sends no reset).
sudo systemd-run --on-active=2 systemctl reboot
EOS
    # Wait for the event (the new kernel answering ssh), with a deadline.
    for _ in $(seq 150); do
        if [ "$(timeout 20 "$S/gssh" "$n" uname -r 2>/dev/null)" = "$KREL" ]; then
            echo "rx-$n: running $KREL"
            return 0
        fi
        sleep 2
    done
    echo "rx-$n: did not come back on $KREL" >&2
    return 1
}
pids=()
for n in $GUESTS; do one "$n" > >(sed "s/^/[rx-$n] /") 2>&1 & pids+=($!); done
rc=0
for p in "${pids[@]}"; do wait "$p" || rc=1; done
exit $rc
