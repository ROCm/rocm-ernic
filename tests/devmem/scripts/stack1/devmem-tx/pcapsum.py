#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""pcapsum.py FILE PORT: summarize TCP frames to PORT in a pcap (Ethernet).

Counts frames from the client towards PORT, those carrying payload, payload
bytes, payloads that are entirely zero bytes, and frames with a bad TCP
checksum (IPv4 only).
"""

import collections
import struct
import sys
from pathlib import Path


def csum(data):
    """The folded 16-bit ones' complement sum of @data (not inverted)."""
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return s


def main():
    """Print the summary of the pcap named by argv[1] for the port in argv[2]."""
    path, port = sys.argv[1], int(sys.argv[2])
    raw = Path(path).read_bytes()
    off = 24
    n = collections.Counter()
    while off + 16 <= len(raw):
        incl = struct.unpack_from("<IIII", raw, off)[2]
        f = raw[off + 16 : off + 16 + incl]
        off += 16 + incl
        if len(f) < 54 or f[12:14] != b"\x08\x00" or f[23] != 6:
            continue
        ihl = (f[14] & 15) * 4
        tcp = f[14 + ihl : 14 + struct.unpack_from("!H", f, 16)[0]]
        sport, dport = struct.unpack_from("!HH", tcp, 0)
        if sport == port:
            n["back"] += 1
            continue
        if dport != port:
            continue
        n["to"] += 1
        n["maxlen"] = max(n["maxlen"], len(f))
        n["syn"] += bool(tcp[13] & 2)
        n["fin"] += bool(tcp[13] & 1)
        n["rst"] += bool(tcp[13] & 4)
        payload = tcp[(tcp[12] >> 4) * 4 :]
        pseudo = f[26:34] + struct.pack("!BBH", 0, 6, len(tcp))
        if csum(pseudo + tcp) != 0xFFFF:
            n["badck"] += 1
        if payload:
            n["pay"] += 1
            n["paybytes"] += len(payload)
            if not any(payload):
                n["zero"] += 1
    print(
        f"PCAP {path}: frames to port {port}: {n['to']} (syn {n['syn']} fin {n['fin']}"
        f" rst {n['rst']}), with payload: {n['pay']},"
        f" payload bytes: {n['paybytes']}, all-zero payloads: {n['zero']},"
        f" bad TCP checksum: {n['badck']},"
        f" longest frame: {n['maxlen']}, frames from port {port}: {n['back']}"
    )


main()
