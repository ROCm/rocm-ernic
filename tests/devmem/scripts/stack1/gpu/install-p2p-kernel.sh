#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Install and boot a "-p2p" test kernel (built by kbuild-p2p.sh) in guests.
#   install-p2p-kernel.sh 7.2.15|7.4-rc3 [GUESTS]      GUESTS default "1"
# Same mechanism as ../install-kernel.sh (debs over 9p, GRUB_TOP_LEVEL in
# zz-devmem.cfg, detached reboot, wait for the release to answer ssh).
# Go back with ../install-kernel.sh 7.2.15|7.4-rc3|stock.
set -euo pipefail
S=$(dirname "$(readlink -f "$0")")/..
FL=${1:?usage: install-p2p-kernel.sh 7.2.15|7.4-rc3 [guests]}
GUESTS=${2:-1}
case "$FL" in
7.2.15) KREL=7.2.15-devmem-p2p ;;
7.4-rc3) KREL=7.4.0-rc3-devmem-kasan-p2p ;;
*) echo "unknown flavour" >&2; exit 2 ;;
esac
for n in $GUESTS; do
    "$S/gssh" "$n" - <<EOS
set -eu
mountpoint -q /mnt/hostfs || sudo mount /mnt/hostfs
if [ ! -e /boot/vmlinuz-$KREL ]; then
    sudo touch /etc/dkms/no-autoinstall-errors
    sudo DEBIAN_FRONTEND=noninteractive dpkg -i \
        /mnt/hostfs/test/kbuild/$FL-p2p/linux-headers-${KREL}_*.deb \
        /mnt/hostfs/test/kbuild/$FL-p2p/linux-image-${KREL}_*.deb >/tmp/dpkg-$KREL.log 2>&1 || echo "dpkg rc=\$? (see /tmp/dpkg-$KREL.log)"
fi
test -e /boot/vmlinuz-$KREL
printf '# written by the devmem PoC harness, gpu/install-p2p-kernel.sh\nGRUB_TOP_LEVEL="/boot/vmlinuz-%s"\nGRUB_DEFAULT=0\n' "$KREL" |
    sudo tee /etc/default/grub.d/zz-devmem.cfg >/dev/null
sudo update-grub 2>&1 | grep -E 'Found linux|error' || true
sudo systemd-run --on-active=2 systemctl reboot
EOS
    for _ in $(seq 150); do
        if [ "$(timeout 20 "$S/gssh" "$n" uname -r 2>/dev/null)" = "$KREL" ]; then
            echo "devmem-$n: running $KREL"
            continue 2
        fi
        sleep 2
    done
    echo "devmem-$n: did not come back on $KREL" >&2
    exit 1
done
