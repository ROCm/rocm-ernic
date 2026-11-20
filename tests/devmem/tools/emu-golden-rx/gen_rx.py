"""Build tests/golden/ionic_eth_rx_frames.h from fields, independently of the emulator.

Checksums are RFC 1071 sums computed here from nothing.  verify_rx.py then
has scapy re-derive them.  Usage: gen_rx.py OUT.h
"""

import struct
import sys


def csum(data):
    """The folded 16-bit ones' complement sum of @data (not inverted)."""
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return total


DMAC = bytes.fromhex("02000000000b")
SMAC = bytes.fromhex("02000000000a")
S4 = bytes([10, 0, 0, 1])
D4 = bytes([10, 0, 0, 2])
S6 = bytes.fromhex("fd000000000000000000000000000001")
D6 = bytes.fromhex("fd000000000000000000000000000002")
SPORT, DPORT = 40000, 5201
TSOPT = bytes.fromhex("0101080a 00000064 000000c8")


def eth(etype, tags=()):
    """An Ethernet header with the VLAN @tags, (TPID, TCI) pairs, and @etype."""
    hdr = DMAC + SMAC
    for tpid, tci in tags:
        hdr += struct.pack("!HH", tpid, tci)
    return hdr + struct.pack("!H", etype)


def ip4(l4, **kw):
    """An IPv4 header and @l4 behind it.

    Keywords: proto (default 6), opts (IP options), frag (flags and
    fragment offset, default DF), total and vihl (to lie about the total
    length or the version and header length), bad (a wrong checksum).
    """
    opts = kw.get("opts", b"")
    proto, frag = kw.get("proto", 6), kw.get("frag", 0x4000)
    ihl = 20 + len(opts)
    total = kw.get("total", ihl + len(l4))
    vihl = kw.get("vihl", 0x40 | (ihl // 4))
    h = (
        struct.pack("!BBHHHBBH4s4s", vihl, 0, total, 0x1234, frag, 64, proto, 0, S4, D4)
        + opts
    )
    c = 0xFFFF ^ csum(h)
    if kw.get("bad", False):
        c ^= 0x0100
    return h[:10] + struct.pack("!H", c) + h[12:] + l4


def ip6(l4, nh=6, plen=None, first=0x60):
    """An IPv6 header, with next header @nh, and @l4 behind it."""
    if plen is None:
        plen = len(l4)
    return struct.pack("!BBHHBB", first, 0, 0, plen, nh, 64) + S6 + D6 + l4


def pseudo(v6, proto, n):
    """The pseudo-header of @n bytes of @proto over IPv6 or IPv4."""
    if v6:
        return S6 + D6 + struct.pack("!IBBBB", n, 0, 0, 0, proto)
    return S4 + D4 + struct.pack("!BBH", 0, proto, n)


def tcp(payload=b"", opts=b"", **kw):
    """A TCP segment with @opts and @payload.

    Keywords: flags (default PSH|ACK), seq, ack, v6 (checksum over the
    IPv6 pseudo-header), doff (to lie about the header length), bad (a
    wrong checksum).
    """
    flags, seq, ack = kw.get("flags", 0x18), kw.get("seq", 1000), kw.get("ack", 2000)
    doff = kw.get("doff", 20 + len(opts))
    h = (
        struct.pack(
            "!HHIIBBHHH", SPORT, DPORT, seq, ack, (doff // 4) << 4, flags, 0xFFFF, 0, 0
        )
        + opts
    )
    seg = h + payload
    c = 0xFFFF ^ csum(pseudo(kw.get("v6", False), 6, len(seg)) + seg)
    if kw.get("bad", False):
        c ^= 0x0001
    return seg[:16] + struct.pack("!H", c) + seg[18:]


def udp(payload, v6=False, mode="good", ulen=None):
    """A UDP datagram; @mode is good, bad or none (no checksum), @ulen its claimed length."""
    n = 8 + len(payload)
    if ulen is None:
        ulen = n
    h = struct.pack("!HHHH", SPORT, DPORT, ulen, 0)
    seg = h + payload
    if mode == "none":
        c = 0
    else:
        c = 0xFFFF ^ csum(pseudo(v6, 17, n) + seg)
        if c == 0:
            c = 0xFFFF
        if mode == "bad":
            c ^= 0x0001
    return seg[:6] + struct.pack("!H", c) + seg[8:]


def pat(n, mul=7, base=3):
    """@n pattern bytes."""
    return bytes((i * mul + base) & 0xFF for i in range(n))


def pad(frame, n=60):
    """@frame padded with zeros to @n bytes."""
    return frame + b"\0" * max(0, n - len(frame))


P16 = bytes(range(0x41, 0x51))
F = []  # (name, comment, bytes)


def add(name, comment, frame):
    """Add a frame to the header."""
    F.append((name, comment, frame))


FRAME_A = eth(0x0800) + ip4(tcp(P16, TSOPT))
add(
    "a",
    "rx-interface.md example A: TCP/IPv4, 12 bytes of TCP options, 16 payload\n"
    " * bytes; the headers end at 66",
    FRAME_A,
)
FRAME_B = pad(eth(0x0800) + ip4(tcp(b"Z", seq=1016)))
add("b", "Example B: TCP/IPv4, one payload byte, padded from 55 bytes to 60", FRAME_B)
add(
    "tcp4_1448",
    "TCP/IPv4, no options, 1448 payload bytes; the headers end at 54",
    eth(0x0800) + ip4(tcp(pat(1448))),
)
add(
    "tcp4_1448_bad",
    "The same with a wrong TCP checksum",
    eth(0x0800) + ip4(tcp(pat(1448), bad=True)),
)
add(
    "udp4_1200",
    "UDP/IPv4, 1200 payload bytes",
    eth(0x0800) + ip4(udp(pat(1200, 13, 5)), proto=17),
)
add(
    "tcp4_vlan",
    "Frame A behind one 802.1Q tag; the headers end at 70",
    eth(0x0800, [(0x8100, 0x2064)]) + ip4(tcp(P16, TSOPT)),
)
add(
    "tcp4_qinq",
    "Frame A behind an 802.1ad and an 802.1Q tag; the headers end at 74",
    eth(0x0800, [(0x88A8, 0x00C8), (0x8100, 0x2064)]) + ip4(tcp(P16, TSOPT)),
)
add(
    "tcp4_vlan3",
    "Frame A behind three tags, one more than the device looks past",
    eth(0x0800, [(0x88A8, 0x00C8), (0x8100, 0x2064), (0x8100, 0x0001)])
    + ip4(tcp(P16, TSOPT)),
)
add(
    "tcp4_ihl6",
    "TCP/IPv4 with a 4-byte IP option (router alert), 16 payload bytes; the\n * headers end at 58",
    eth(0x0800) + ip4(tcp(P16), opts=bytes.fromhex("94040000")),
)
add(
    "tcp4_ihl15",
    "TCP/IPv4 with 40 bytes of IP options and 12 of TCP options, 16 payload\n"
    " * bytes; the headers end at 106",
    eth(0x0800) + ip4(tcp(P16, TSOPT), opts=b"\x01" * 40),
)
add(
    "tcp6",
    "TCP/IPv6, no options, 32 payload bytes; the headers end at 74",
    eth(0x86DD) + ip6(tcp(pat(32, 5, 1), v6=True)),
)
add(
    "tcp6_opts",
    "TCP/IPv6, 12 bytes of TCP options, 16 payload bytes; the headers end\n * at 86",
    eth(0x86DD) + ip6(tcp(P16, TSOPT, v6=True)),
)
add(
    "tcp6_vlan",
    "TCP/IPv6 behind one 802.1Q tag, 8 payload bytes; the headers end at 78",
    eth(0x86DD, [(0x8100, 0x0064)]) + ip6(tcp(P16[:8], v6=True)),
)
add(
    "syn4",
    "A TCP/IPv4 SYN with 20 bytes of options and no payload, 74 bytes",
    eth(0x0800)
    + ip4(
        tcp(
            b"",
            bytes.fromhex("020405b4 0402 080a0000006400000000 01 030307"),
            flags=0x02,
            ack=0,
        )
    ),
)
add(
    "ack4",
    "A TCP/IPv4 ACK with no payload, as a virtual wire carries it: 54\n * bytes, not padded",
    eth(0x0800) + ip4(tcp(b"", flags=0x10)),
)
add(
    "ack4_pad",
    "The same padded to 60 bytes",
    pad(eth(0x0800) + ip4(tcp(b"", flags=0x10))),
)
add(
    "tcp6_hbh",
    "TCP/IPv6 behind a hop-by-hop options header",
    eth(0x86DD) + ip6(bytes.fromhex("0600010400000000") + tcp(P16, v6=True), nh=0),
)
add(
    "tcp6_frag",
    "TCP/IPv6 behind a fragment header",
    eth(0x86DD) + ip6(bytes.fromhex("0600000000001234") + tcp(P16, v6=True), nh=44),
)
add(
    "frag4_first",
    "The first fragment of a TCP/IPv4 datagram: more fragments, offset 0",
    eth(0x0800) + ip4(tcp(P16), frag=0x2000),
)
add(
    "frag4_later",
    "A later fragment of one: offset 185",
    eth(0x0800) + ip4(pat(32, 3, 9), frag=0x00B9),
)
add("udp4", "UDP/IPv4, 18 payload bytes", eth(0x0800) + ip4(udp(pat(18)), proto=17))
add(
    "udp4_nocsum",
    "The same with no UDP checksum",
    eth(0x0800) + ip4(udp(pat(18), mode="none"), proto=17),
)
add(
    "udp4_bad",
    "The same with a wrong UDP checksum",
    eth(0x0800) + ip4(udp(pat(18), mode="bad"), proto=17),
)
add(
    "udp4_pad",
    "UDP/IPv4, 4 payload bytes, padded from 46 bytes to 60",
    pad(eth(0x0800) + ip4(udp(pat(4)), proto=17)),
)
add(
    "udp4_lenmis",
    "UDP/IPv4 whose UDP length, 20, is not the 26 bytes the IP header gives it",
    eth(0x0800) + ip4(udp(pat(18), ulen=20), proto=17),
)
add(
    "udp4_short",
    "IPv4 naming UDP with 6 bytes behind it, less than a UDP header, though\n"
    " * they say a length of 6; then padding to 60 bytes that is not zero",
    eth(0x0800) + ip4(bytes.fromhex("9c4014510006"), proto=17) + b"\xff" * 20,
)
add(
    "udp6",
    "UDP/IPv6, 18 payload bytes",
    eth(0x86DD) + ip6(udp(pat(18), v6=True), nh=17),
)
add(
    "udp6_bad",
    "The same with a wrong UDP checksum",
    eth(0x86DD) + ip6(udp(pat(18), v6=True, mode="bad"), nh=17),
)
add(
    "udp6_nocsum",
    "The same with a zero UDP checksum",
    eth(0x86DD) + ip6(udp(pat(18), v6=True, mode="none"), nh=17),
)
icmp = struct.pack("!BBHHH", 8, 0, 0, 0x1234, 1) + pat(16)
icmp = icmp[:2] + struct.pack("!H", 0xFFFF ^ csum(icmp)) + icmp[4:]
add("icmp4", "An ICMP echo request", eth(0x0800) + ip4(icmp, proto=1))
add(
    "arp",
    "An ARP request, padded from 42 bytes to 60",
    pad(
        bytes.fromhex("ffffffffffff")
        + SMAC
        + bytes.fromhex("0806 0001 0800 06 04 0001")
        + SMAC
        + S4
        + bytes(6)
        + D4
    ),
)
add(
    "other61",
    "An experimental ethertype, 61 bytes: an odd number to sum",
    eth(0x88B5) + pat(47, 11, 0x80),
)
add("runt13", "13 bytes: not even an Ethernet header", (DMAC + SMAC + b"\x08\x00")[:13])
add("eth14", "An Ethernet header naming IPv4 and nothing behind it", eth(0x0800))
add(
    "vlan_trunc",
    "An 802.1Q ethertype with half a tag behind it",
    DMAC + SMAC + bytes.fromhex("81000064"),
)
add("ip4_short", "19 bytes of an IPv4 header", (eth(0x0800) + ip4(tcp(P16)))[:33])
add(
    "ip4_ihl4",
    "IPv4 with a header length of 16 bytes, less than the least",
    eth(0x0800) + ip4(tcp(P16), vihl=0x44),
)
add(
    "ip4_ihl_over",
    "IPv4 with a header length of 60 bytes in a frame with 56 behind the\n * Ethernet header",
    eth(0x0800) + ip4(tcp(P16), vihl=0x4F),
)
add(
    "ip4_ver6",
    "The IPv4 ethertype over a header that says version 6",
    eth(0x0800) + ip4(tcp(P16), vihl=0x65),
)
add(
    "ip6_ver4",
    "The IPv6 ethertype over a header that says version 4",
    eth(0x86DD) + ip6(tcp(P16, v6=True), first=0x40),
)
add(
    "ip6_short",
    "39 bytes of an IPv6 header",
    (eth(0x86DD) + ip6(tcp(P16, v6=True)))[:53],
)
add(
    "ip6_plen_over",
    "IPv6 whose payload length is one more than the frame has",
    eth(0x86DD) + ip6(tcp(P16, v6=True), plen=37),
)
add("ip4_trunc", "Frame A cut 12 bytes short of its IP total length", FRAME_A[:70])
t = eth(0x0800) + ip4(tcp(P16), total=16)
add("ip4_total_under", "IPv4 with a total length of 16, less than its own header", t)
add(
    "tcp4_short",
    "IPv4 naming TCP with 12 bytes behind it, less than a TCP header",
    eth(0x0800) + ip4(pat(12, 3, 1)),
)
add(
    "tcp4_doff4",
    "TCP/IPv4 with a good checksum and a TCP header length of 16",
    eth(0x0800) + ip4(tcp(P16, doff=16)),
)
add(
    "tcp4_doff_over",
    "TCP/IPv4 with a good checksum and a TCP header length of 40 in a\n * 36-byte segment",
    eth(0x0800) + ip4(tcp(P16, doff=40)),
)
add(
    "ip4_badcsum",
    "Frame A with a wrong IPv4 header checksum",
    eth(0x0800) + ip4(tcp(P16, TSOPT), bad=True),
)
add(
    "tcp4_badcsum",
    "Frame A with a wrong TCP checksum",
    eth(0x0800) + ip4(tcp(P16, TSOPT, bad=True)),
)
add(
    "tcp6_badcsum",
    "TCP/IPv6 with a wrong TCP checksum",
    eth(0x86DD) + ip6(tcp(P16, TSOPT, v6=True, bad=True)),
)

# The spec's own bytes for A and B.
SPEC_A = bytes.fromhex(
    "02 00 00 00 00 0b 02 00 00 00 00 0a 08 00 45 00 00 44 12 34 40 00 40 06 14 7e"
    " 0a 00 00 01 0a 00 00 02 9c 40 14 51 00 00 03 e8 00 00 07 d0 80 18 ff ff 62 e3"
    " 00 00 01 01 08 0a 00 00 00 64 00 00 00 c8 41 42 43 44 45 46 47 48 49 4a 4b 4c"
    " 4d 4e 4f 50"
)
SPEC_B = bytes.fromhex(
    "02 00 00 00 00 0b 02 00 00 00 00 0a 08 00 45 00 00 29 12 34 40 00 40 06 14 99"
    " 0a 00 00 01 0a 00 00 02 9c 40 14 51 00 00 03 f8 00 00 07 d0 50 18 ff ff 85 6f"
    " 00 00 5a 00 00 00 00 00"
)
assert SPEC_A == FRAME_A and SPEC_B == FRAME_B
assert csum(FRAME_A[14:]) == 0xEBC6 and csum(FRAME_B[14:]) == 0xEBE1

HEADER = """/*
 * Frames for tests/test_ionic_eth_rx.c: what the emulated NIC receives in
 * header-split mode, and for each the ones-complement sum of everything
 * behind its Ethernet header (ERX_..._CSUM), which its completion reports.
 *
 * The frames are from 02:00:00:00:00:0a to 02:00:00:00:00:0b, 10.0.0.1 to
 * 10.0.0.2 or fd00::1 to fd00::2, ports 40000 to 5201.  The bytes and the
 * sums were not produced by the emulator: a script built each frame from
 * its fields, with the RFC 1071 checksums computed from nothing, and scapy
 * then parsed each one and recomputed its IPv4, TCP and UDP checksums: to
 * the same bytes for a frame described as good, and to different ones for
 * one described as wrong.  Change a frame only by hand, with the same
 * check.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef IONIC_ETH_RX_FRAMES_H
#define IONIC_ETH_RX_FRAMES_H

#include <stdint.h>
"""


def main():
    """Write the header named by argv[1] and print each frame's size and sum."""
    with open(sys.argv[1], "w", encoding="utf-8") as out:
        out.write(HEADER)
        for name, comment, frame in F:
            out.write(f"\n/* {comment} */\n")
            out.write(f"static const uint8_t erx_{name}[{len(frame)}] = {{\n")
            for i in range(0, len(frame), 12):
                row = " ".join(f"0x{x:02x}," for x in frame[i : i + 12])
                out.write("    " + row + "\n")
            out.write("};\n")
            out.write(f"#define ERX_{name.upper()}_CSUM 0x{csum(frame[14:]):04x}U\n")
        out.write("\n#endif /* IONIC_ETH_RX_FRAMES_H */\n")
    for name, _, frame in F:
        print(name, len(frame), f"{csum(frame[14:]):04x}")


main()
