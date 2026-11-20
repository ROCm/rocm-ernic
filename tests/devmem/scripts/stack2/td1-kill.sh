#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# shellcheck source=tests/devmem/scripts/stack2/tdlib.sh
. "$(dirname "$(readlink -f "$0")")/tdlib.sh"
hdr "T1: SIGKILL ncdevmem mid-stream (x3)"
for _ in 1 2 3; do
    "$S/devmem-rx.sh" start 1 | tail -n 1; stream_bg 2000000000; wait_rx_bytes 20000000
    g 1 'sudo pkill -9 -x ncdevmem; echo "killed ncdevmem rc=$?"'; "$S/devmem-rx.sh" summary 1 | head -n 2
    wait_no_dmabuf_pool 1
    # shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
    g 2 'for i in $(seq 100); do [ -e /tmp/sender.rc ] && break; sleep 0.2; done; cat /tmp/sender.rc 2>/dev/null || { echo "sender still running; killing"; pkill socat; }'
    echo "   counters: $("$S/counters.sh" 1)"
done
health
hdr "T1: first light again after the kills"; first_light
