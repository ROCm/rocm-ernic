#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Run in a guest: (re)start the gt.py receiver on port 5400 and wait for it.
set -u
[ -e /tmp/gt-rx.pid ] && kill "$(cat /tmp/gt-rx.pid)" 2>/dev/null
for _ in $(seq 50); do ss -ltnH 'sport = :5400' | grep -q . || break; sleep 0.1; done
: >/tmp/gt-rx.log
nohup python3 /mnt/hostfs/test/scripts/emu-offloads/gt.py rx 5400 /tmp/gt-rx.log >/tmp/gt-rx.out 2>&1 &
echo $! >/tmp/gt-rx.pid
for _ in $(seq 50); do ss -ltnH 'sport = :5400' | grep -q . && exit 0; sleep 0.1; done
echo "receiver did not start" >&2; cat /tmp/gt-rx.out >&2; exit 1
