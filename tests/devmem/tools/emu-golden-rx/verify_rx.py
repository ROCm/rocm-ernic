# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""Check tests/golden/ionic_eth_rx_frames.h with scapy.

Independent of the emulator and of gen_rx.py.
Usage: verify_rx.py tests/golden/ionic_eth_rx_frames.h   (needs scapy)
"""

import re
import sys
from pathlib import Path

# pylint: disable-next=import-error # scapy is not installed where CI runs pylint
from scapy.all import IP, TCP, UDP, Ether, raw

src = Path(sys.argv[1]).read_text(encoding="utf-8")
arrs = {}
for m in re.finditer(
    r"static const uint8_t erx_(\w+)\[(\d+)\] = \{(.*?)\};\n#define ERX_\w+_CSUM 0x([0-9a-f]{4})U",
    src,
    re.S,
):
    frame = bytes(int(x, 16) for x in re.findall(r"0x[0-9a-f]{2}", m.group(3)))
    assert len(frame) == int(m.group(2)), m.group(1)
    arrs[m.group(1)] = (frame, int(m.group(4), 16))


def ones(data):
    """The folded 16-bit ones' complement sum of @data (not inverted)."""
    s = 0
    for i in range(0, len(data), 2):
        s += (data[i] << 8) | (data[i + 1] if i + 1 < len(data) else 0)
    while s > 0xFFFF:
        s = (s & 0xFFFF) + (s >> 16)
    return s


# which checksums each frame should carry correctly: i = IPv4 header, t = TCP,
# u = UDP; upper case = wrong
EXPECT = {
    "a": "it",
    "b": "it",
    "tcp4_1448": "it",
    "tcp4_1448_bad": "iT",
    "udp4_1200": "iu",
    "tcp4_vlan": "it",
    "tcp4_qinq": "it",
    "tcp4_vlan3": "it",
    "tcp4_ihl6": "it",
    "tcp4_ihl15": "it",
    "tcp6": "t",
    "tcp6_opts": "t",
    "tcp6_vlan": "t",
    "syn4": "it",
    "ack4": "it",
    "ack4_pad": "it",
    "tcp6_hbh": "t",
    "tcp6_frag": "",
    "frag4_first": "i",
    "frag4_later": "i",
    "udp4": "iu",
    "udp4_nocsum": "i",
    "udp4_bad": "iU",
    "udp4_pad": "iu",
    "udp4_lenmis": "i",
    "udp4_short": "i",
    "udp6": "u",
    "udp6_bad": "U",
    "udp6_nocsum": "",
    "icmp4": "i",
    "arp": "",
    "other61": "",
    "runt13": "",
    "eth14": "",
    "vlan_trunc": "",
    "ip4_short": "",
    "ip4_ihl4": "",
    "ip4_ihl_over": "",
    "ip4_ver6": "",
    "ip6_ver4": "",
    "ip6_short": "",
    "ip6_plen_over": "",
    "ip4_trunc": "i",
    "ip4_total_under": "i",
    "tcp4_short": "i",
    "tcp4_doff4": "i",
    "tcp4_doff_over": "i",
    "ip4_badcsum": "It",
    "tcp4_badcsum": "iT",
    "tcp6_badcsum": "T",
}
bad = 0
assert set(EXPECT) == set(arrs), set(EXPECT) ^ set(arrs)
for name, (b, want_sum) in arrs.items():
    ok = ones(b[14:]) == want_sum if len(b) >= 14 else want_sum == 0
    notes = []
    for code, layer in (("i", IP), ("t", TCP), ("u", UDP)):
        if code not in EXPECT[name].lower():
            continue
        p = Ether(b)
        if not p.haslayer(layer):
            ok = False
            notes.append(f"no {layer.__name__} layer")
            continue
        have = p[layer].chksum
        del p[layer].chksum
        q = Ether(raw(p))
        same = q[layer].chksum == have or (
            layer is UDP and {q[layer].chksum, have} == {0, 0xFFFF}
        )
        good = code in EXPECT[name]
        if same != good:
            ok = False
        notes.append(f"{layer.__name__} {'good' if same else 'wrong'}")
    print(
        ("ok  " if ok else "BAD ") + name, len(b), f"{want_sum:04x}", ", ".join(notes)
    )
    bad += not ok
print(f"{len(arrs)} frames, {bad} bad")
sys.exit(1 if bad else 0)
