# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""Build the review round's TSO frames for tests/golden/ionic_eth_tx_frames.h.

Each packet is built by scapy from its fields; each expected segment is built
by scapy as a packet of its own (never by cutting up emulator output), and its
TCP checksum is then recomputed here from nothing (RFC 1071 over the IPv6
pseudo-header with the IPv6 header's own destination) and must agree.
"""

import struct
import sys
from pathlib import Path

# pylint: disable-next=import-error # scapy is not installed where CI runs pylint
from scapy.all import (
    TCP,
    Dot1Q,
    Ether,
    IPv6,
    IPv6ExtHdrDestOpt,
    IPv6ExtHdrHopByHop,
    IPv6ExtHdrRouting,
    PadN,
    Raw,
    raw,
)

SRC, DST = "02:00:00:00:00:01", "02:00:00:00:00:02"
S6, D6 = "fd00::2", "fd00::1"


def csum(data):
    """The RFC 1071 checksum of @data."""
    if len(data) % 2:
        data += b"\0"
    total = sum(int.from_bytes(data[i : i + 2], "big") for i in range(0, len(data), 2))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return ~total & 0xFFFF


def check_tcp6(frame, l3, l4):
    """Assert @frame's TCP checksum and IPv6 payload length, computed from nothing."""
    ip = frame[l3 : l3 + 40]
    tcp = bytearray(frame[l4:])
    got = struct.unpack("!H", tcp[16:18])[0]
    tcp[16:18] = b"\0\0"
    pseudo = ip[8:40] + struct.pack("!IHBB", len(tcp), 0, 0, 6)
    want = csum(pseudo + bytes(tcp))
    assert got == want, (hex(got), hex(want))
    assert struct.unpack("!H", ip[4:6])[0] == len(frame) - l3 - 40


def payload(n, seed):
    """@n payload bytes that depend on @seed."""
    return bytes((seed + 7 * i) & 0xFF for i in range(n))


def packet(case, seq, flags, data):
    """The scapy packet of @case with this sequence number, flags and payload."""
    pkt = case["l2"]() / IPv6(src=S6, dst=D6, hlim=64)
    for ext in case["exts"]():
        pkt = pkt / ext
    return (
        pkt
        / TCP(
            sport=40000,
            dport=5201,
            seq=seq,
            ack=0x01020304,
            flags=flags,
            window=512,
            options=list(case.get("topts", ())),
        )
        / Raw(data)
    )


def build(case):
    """The input template and expected segments of @case, as (name, bytes, comment).

    l2: layers before IPv6; exts: extension header layers; hdr_len, mss, npay:
    header bytes, segment size and payload bytes; seq, flags: of the TCP
    header; seed: of the payload; topts: TCP options.
    """
    topts = case.get("topts", ())
    data = payload(case["npay"], case["seed"])
    l3 = len(raw(case["l2"]()))
    full = bytearray(raw(packet(case, case["seq"], case["flags"], data)))
    l4 = case["hdr_len"] - 20 - 4 * (len(topts) // 4)
    assert len(topts) % 4 == 0
    assert len(full) == case["hdr_len"] + case["npay"], len(full)
    # The template the driver hands over: no payload length, no checksum.
    full[l3 + 4 : l3 + 6] = b"\0\0"
    full[l4 + 16 : l4 + 18] = b"\0\0"
    out = [(f"{case['name']}_in", bytes(full), case["comment"])]
    off, i = 0, 0
    while off < case["npay"]:
        chunk = data[off : off + case["mss"]]
        flags = case["flags"]
        if off + len(chunk) != case["npay"]:
            flags &= ~0x09
        if i:
            flags &= ~0x80
        seg = raw(packet(case, (case["seq"] + off) & 0xFFFFFFFF, flags, chunk))
        check_tcp6(seg, l3, l4)
        assert seg[:12] == bytes(full[:12])
        assert len(seg) == case["hdr_len"] + len(chunk)
        out.append((f"{case['name']}_seg{i}", seg, None))
        off += len(chunk)
        i += 1
    return out


CASES = [
    {
        "name": "etx_tso6_qinq",
        "comment": "TSO over IPv6 behind an 802.1ad tag: 78 bytes, then 20 of data",
        "l2": lambda: Ether(src=SRC, dst=DST, type=0x88A8)
        / Dot1Q(vlan=200, prio=3, type=0x86DD),
        "exts": lambda: [],
        "hdr_len": 78,
        "mss": 16,
        "npay": 20,
        "seq": 0x00002710,
        "flags": 0x18,
        "seed": 0x31,
    },
    {
        "name": "etx_tso6_dst_rt",
        "comment": "TSO over IPv6 with a destination-options header and a routing\n"
        " * header with one address and no segments left: 106 bytes, then 15 of\n"
        " * data",
        "l2": lambda: Ether(src=SRC, dst=DST),
        "exts": lambda: [
            IPv6ExtHdrDestOpt(options=[PadN(optdata=b"\0" * 4)]),
            IPv6ExtHdrRouting(type=0, segleft=0, addresses=["fd00::9"]),
        ],
        "hdr_len": 106,
        "mss": 10,
        "npay": 15,
        "seq": 0xFFFFFFFA,
        "flags": 0x99,
        "seed": 0x52,
    },
    {
        "name": "etx_tso6_hbh254",
        "comment": "TSO over IPv6 with a 176-byte hop-by-hop header and four bytes of\n"
        " * TCP options: 254 bytes of headers, the most that fit the 256 the\n"
        " * device takes, then 10 of data",
        "l2": lambda: Ether(src=SRC, dst=DST),
        "exts": lambda: [IPv6ExtHdrHopByHop(options=[PadN(optdata=b"\0" * 172)])],
        "hdr_len": 254,
        "mss": 6,
        "npay": 10,
        "seq": 0x00004E20,
        "flags": 0x10,
        "seed": 0x73,
        "topts": [("NOP", None)] * 4,
    },
]

MARKER = "\n#endif /* IONIC_ETH_TX_FRAMES_H */"


def main():
    """Append the frames to the header named by argv[1], before its last #endif."""
    arrs = [arr for case in CASES for arr in build(case)]
    lines = []
    for name, frame, comment in arrs:
        lines.append("")
        if comment:
            lines.append(f"/* {comment} */")
        lines.append(f"static const uint8_t {name}[{len(frame)}] = {{")
        for i in range(0, len(frame), 12):
            lines.append("    " + " ".join(f"0x{x:02x}," for x in frame[i : i + 12]))
        lines.append("};")
    path = Path(sys.argv[1])
    header = path.read_text(encoding="utf-8")
    assert header.count(MARKER) == 1 and "etx_tso6_qinq" not in header
    path.write_text(
        header.replace(MARKER, "\n".join(lines) + "\n" + MARKER), encoding="utf-8"
    )
    for name, frame, _ in arrs:
        print(name, len(frame))


main()
