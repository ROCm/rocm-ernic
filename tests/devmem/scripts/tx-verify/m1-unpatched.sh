#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# M1 with own tool: bind attempt on whatever ionic is loaded (run right after a boot = the kernel's unpatched in-tree ionic)
# shellcheck source=tests/devmem/scripts/tx-verify/v.sh
. "$(dirname "$(readlink -f "$0")")/v.sh"
for n in 1 2; do "$G" $n "ip -4 -o addr show dev $IF | grep -q 10.213.100.1$n || sudo ip addr add 10.213.100.1$n/24 dev $IF"; done
g2 "rm -f /tmp/vrx.log; nohup python3 /usr/local/bin/vrx.py 6600 --idle 6 >/tmp/vrx.log 2>&1 & sleep 1"
# shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
g1 'echo "kernel $(uname -r); ionic taint [$(cat /sys/module/ionic/taint)] build-id $(sudo xxd -p /sys/module/ionic/notes/.note.gnu.build-id | tr -d "\n" | cut -c33-); installed ionic.ko: $(readelf -n /lib/modules/$(uname -r)/kernel/drivers/net/ethernet/pensando/ionic/ionic.ko* 2>/dev/null | grep "Build ID" | tr -s " ")"; ethtool -k rocm-ernic0 | grep -E "^(tx-checksumming|scatter-gather|tcp-segmentation-offload):" | tr -s " \n" " "; echo; sudo ethtool -S rocm-ernic0 | grep -c unreadable_drop; sudo vtx -S 4m -R 4 rocm-ernic0 '"$IP2"' 6600 d:1:1m; echo "vtx exit status: $?"'  # pre-commit: ignore
g2 "sleep 7; cat /tmp/vrx.log; pkill -f vrx.py"
