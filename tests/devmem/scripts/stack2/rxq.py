#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""rxq.py SRC DST DPORT NQUEUES WANTQ [START]: first source port >= START
(default 40000) for which the emulator's FNV-1a flow hash (rx-interface.md
T3) puts TCP SRC:port -> DST:DPORT on queue WANTQ."""

import socket
import sys


def fnv1a(b, h=0x811C9DC5):
    """32-bit FNV-1a of the bytes @b, continuing from @h."""
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h


src, dst = socket.inet_aton(sys.argv[1]), socket.inet_aton(sys.argv[2])
dport, n, want = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
p = int(sys.argv[6]) if len(sys.argv) > 6 else 40000
while (
    fnv1a(src + dst + bytes([6]) + p.to_bytes(2, "big") + dport.to_bytes(2, "big")) % n
    != want
):
    p += 1
print(p)
