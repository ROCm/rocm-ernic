# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""Check tests/golden/ionic_eth_tx_frames.h with scapy, independently of the emulator.

Usage: verify.py tests/golden/ionic_eth_tx_frames.h   (needs scapy)
"""

import re
import sys
from pathlib import Path

# pylint: disable-next=import-error # scapy is not installed where CI runs pylint
from scapy.all import IP, TCP, UDP, Dot1Q, Ether, IPv6, raw


def load(path):
    """The byte arrays of the header @path, by name."""
    src = Path(path).read_text(encoding="utf-8")
    found = {}
    for m in re.finditer(
        r"static const uint8_t (\w+)\[(\d+)\] = \{(.*?)\};", src, re.S
    ):
        data = bytes(int(x, 16) for x in re.findall(r"0x[0-9a-f]{2}", m.group(3)))
        assert len(data) == int(m.group(2)), m.group(1)
        found[m.group(1)] = data
    return found


arrs = load(sys.argv[1])


def recomputed(frame):
    """@frame with its checksums recomputed by scapy, and scapy's parse of it."""
    pkt = Ether(frame)
    for layer in (IP, TCP, UDP):
        if pkt.haslayer(layer):
            del pkt[layer].chksum
    return raw(Ether(raw(pkt))), pkt


def report(ok, *what):
    """Print one result line; returns 1 if it is bad, for counting."""
    print(("ok  " if ok else "BAD ") + str(what[0]), *what[1:])
    return 0 if ok else 1


def check_frames():
    """Every output frame's checksums are what scapy computes. Returns the bad count."""
    bad = 0
    for name, frame in sorted(arrs.items()):
        if name.endswith("_in") or name in ("etx_plain60", "etx_sg_v0", "etx_sg_v1"):
            continue
        again, pkt = recomputed(frame)
        # scapy leaves a computed UDP 0 as 0xffff too
        bad += report(again == frame, name, len(frame), pkt.summary())
    return bad


def seg_ok(tin, seg, want, pos):
    """Whether @seg is the segment of the TSO input @tin that carries @want.

    @pos is (header length, segment index, payload offset, whether the last).
    """
    hdr, i, off, last = pos
    pkt = Ether(seg)
    flags = int(tin[TCP].flags)
    if not last:
        flags &= ~0x09
    if i:
        flags &= ~0x80
    ok = (
        seg[hdr:] == want
        and len(seg) == hdr + len(want)
        and pkt[TCP].seq == (tin[TCP].seq + off) & 0xFFFFFFFF
        and int(pkt[TCP].flags) == flags
    )
    l2 = 14 + (4 if pkt.haslayer(Dot1Q) else 0)
    if pkt.haslayer(IP):
        return (
            ok
            and pkt[IP].id == (tin[IP].id + i) & 0xFFFF
            and pkt[IP].len == len(seg) - l2
        )
    return ok and pkt[IPv6].plen == len(seg) - l2 - 40


def tso(name, hdr, mss, segs):
    """Check the segments of the TSO case @name. Returns the bad count."""
    inp = arrs[name + "_in"]
    pay = inp[hdr:]
    fix = bytearray(inp)
    if name == "etx_tso6_hbh":
        # the template's 0 reads as a jumbogram
        fix[18:20] = (len(inp) - 54).to_bytes(2, "big")
    tin = Ether(bytes(fix))
    off = bad = 0
    for i, size in enumerate(segs):
        seg = arrs[f"{name}_seg{i}"]
        want = pay[off : off + mss]
        ok = len(want) == size and seg_ok(
            tin, seg, want, (hdr, i, off, i == len(segs) - 1)
        )
        bad += report(ok, f"{name} seg{i}", len(want))
        off += len(want)
    if off != len(pay):
        print("BAD", name, "payload not covered")
        bad += 1
    return bad


def csum_partial():
    """CSUM_PARTIAL: in and out differ only in the checksum field. Returns the bad count."""
    bad = 0
    for name, start, field in (
        ("tcp4", 34, 16),
        ("udp4", 34, 6),
        ("udp4_zero", 34, 6),
        ("tcp6", 54, 16),
        ("udp6", 54, 6),
    ):
        a = bytearray(arrs[f"etx_cp_{name}_in"])
        b = bytearray(arrs[f"etx_cp_{name}_out"])
        a[start + field : start + field + 2] = b"\0\0"
        b[start + field : start + field + 2] = b"\0\0"
        bad += report(a == b, "cp " + name)
    return bad


sys.exit(
    1
    if check_frames()
    + tso("etx_tso4", 54, 8, [8, 8, 4])
    + tso("etx_tso6_vlan", 78, 16, [16, 16, 1])
    + tso("etx_tso6_hbh", 82, 6, [6, 4])
    + tso("etx_tso4_opts", 70, 1000, [5])
    + csum_partial()
    else 0
)
