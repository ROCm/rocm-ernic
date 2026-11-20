#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""udpsum.py PCAP [fixed]: check the UDP echo test's datagrams in a capture.
Requests (dst port 7777) and echoes (src port 7777) carry an 8-byte sequence
number then the pattern (i*131+7+seq*29)&0xff ("fixed": (i*131+7)&0xff, the
first runs). Reports, per direction, how many frames differ from the pattern
and where, and whether the wrong bytes are another datagram's (same offsets,
other sequence number)."""

import struct
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
FIXED = len(sys.argv) > 2 and sys.argv[2] == "fixed"
other = 0
off = 24
stats = {"req": [0, 0], "echo": [0, 0]}
shown = 0
while off + 16 <= len(data):
    ts, tu, cap, wire = struct.unpack_from("<IIII", data, off)
    p = data[off + 16 : off + 16 + cap]
    off += 16 + cap
    if len(p) < 42 or p[12:14] != b"\x08\x00" or p[23] != 17:
        continue
    ihl = (p[14] & 15) * 4
    u = 14 + ihl
    sp, dp, ulen = struct.unpack_from("!HHH", p, u)
    if 7777 not in (sp, dp):
        continue
    kind = "req" if dp == 7777 else "echo"
    pl = p[u + 8 : u + ulen]
    stats[kind][0] += 1
    seq = struct.unpack("!Q", pl[:8])[0] if len(pl) >= 8 else 0
    k = 0 if FIXED else seq * 29
    exp = bytes((i * 131 + 7 + k) & 0xFF for i in range(len(pl)))
    diff = [i for i in range(8, len(pl)) if pl[i] != exp[i]]
    short = cap < wire or len(pl) != ulen - 8
    if diff or short:
        stats[kind][1] += 1
        # is the wrong run the same offsets of some other datagram?
        if diff and not FIXED:
            a, b = diff[0], diff[-1] + 1
            d = (pl[a] - ((a * 131 + 7) & 0xFF)) & 0xFF
            if all(pl[i] == (i * 131 + 7 + d) & 0xFF for i in range(a, b)):
                other += 1
        if shown < 12:
            shown += 1
            runs = []
            for i in diff:
                if runs and i == runs[-1][1] + 1:
                    runs[-1][1] = i
                else:
                    runs.append([i, i])
            print(
                f"  BAD {kind} seq={seq} t={ts}.{tu:06d} wire={wire} cap={cap}"
                f" payload={len(pl)} differing bytes={len(diff)}"
                f" runs(payload offsets)={[(a, b - a + 1) for a, b in runs[:8]]}"
            )
            if diff:
                a = diff[0]
                print(
                    f"      at {a}: got {pl[a:a+24].hex()} want {exp[a:a+24].hex()}"
                    f" zero-bytes-in-diff={sum(1 for i in diff if pl[i] == 0)}"
                )
print(
    f"UDPSUM {sys.argv[1].split('/')[-1]}: requests={stats['req'][0]}"
    f" bad={stats['req'][1]}; echoes={stats['echo'][0]} bad={stats['echo'][1]};"
    " bad frames whose wrong bytes are another datagram's bytes at the same"
    f" offsets: {other}"
)
