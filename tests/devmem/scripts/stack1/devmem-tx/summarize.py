#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""One line per CASE of a devmem-tx evidence file: results and the checks."""

import re
import sys
from pathlib import Path


def first(pattern, blk, default="-"):
    """The first group of @pattern's first match in @blk, or @default."""
    m = re.search(pattern, blk)
    return m.group(1) if m else default


def change(g1, key):
    """The signed change of counter @key in guest 1's section @g1, "0" if none."""
    return first(re.escape(key) + r"\s+\d+ -> \d+\s+\(([+-]\d+)\)", g1, "0")


def max_frags(blk):
    """The largest frag count in the case's unreadable-skb histogram, or "-"."""
    if "@unreadable_nr_frags:" not in blk:
        return "-"
    hist = blk.split("@unreadable_nr_frags:")[1].split("\n@x")[0]
    frags = [int(x) for x in re.findall(r"^\[(\d+)\] +\d+ \|", hist, flags=re.M)]
    return max(frags) if frags else "-"


def guest1(blk):
    """The part of the case that lists guest 1's and emulator 1's counters."""
    if "--- counters changed, guest 1" not in blk:
        return ""
    after = blk.split("--- counters changed, guest 1")[1]
    return after.split("--- counters changed, guest 2")[0]


def summarize(blk):
    """The summary line of one case, whose text after the CASE marker is @blk."""
    rx = re.search(r"RESULT rx (\S+) bytes=(\d+) expected=(\d+).*mismatches=(\d+)", blk)
    rxv = rx.groups() if rx else ("-",) * 4
    iov = re.findall(r"@unreadable_frag0_netmem_is_net_iov\[(\d)\]: (\d+)", blk)
    g1 = guest1(blk)
    inbind = sum(
        int(x) for x in re.findall(r"DMA-API error_count during bind: (\d+)", blk)
    )
    other = int(first(r"dma-api\.error_count\s+\d+ -> \d+\s+\(\+(\d+)\)", g1, "0"))
    always = re.search(r"always shown: (.*)", g1)
    drops = dict(re.findall(r"(\S+) (\d+);", always.group(1))) if always else {}
    linearize = sum(int(change(g1, f"ethtool.tx_{q}_linearize")) for q in range(4))
    tx = first(r"RESULT tx (\S+)", blk)
    skbs = first(r"@xmit_unreadable_skbs: (\d+)", blk, 0)
    nbytes = first(r"@xmit_unreadable_payload_bytes: (\d+)", blk, 0)
    nofrags = first(r"@unreadable_with_no_frags: (\d+)", blk, 0)
    pfn = first(r"PFN-CHECK.*=> (\S+)", blk)
    return (
        f"{blk.split()[0]}: tx={tx} rx={rxv[0]}"
        f" rx_bytes={rxv[1]}/{rxv[2]} mismatches={rxv[3]}"
        f" unreadable_skbs={skbs}"
        f" unreadable_bytes={nbytes}"
        f" frag0_net_iov={dict(iov)}"
        f" no_frags={nofrags}"
        f" max_frags={max_frags(blk)}"
        f" pfn={pfn}"
        f" d(tx_unreadable_drop)={change(g1, 'ethtool.tx_unreadable_drop')}"
        f" d(netdev.tx_dropped)={change(g1, 'netdev.rocm-ernic0.tx_dropped')}"
        f" d(linearize)={linearize}"
        f" emu_err={drops.get('emu.tx_dma_error')}"
        f"/{drops.get('emu.tx_desc_data_error')}"
        f"/{drops.get('emu.tx_desc_fetch_error')}"
        f" dma_api_other={other - inbind}"
        f" d(dmesg_splat_lines)={change(g1, 'dmesg.splat_lines')}"
        f" dmabuf_objs={drops.get('dmabuf.objects')}"
        f" retrans={change(g1, 'tcp.TcpRetransSegs')}"
    )


for path in sys.argv[1:]:
    text = Path(path).read_text(errors="replace", encoding="utf-8")
    for block in re.split(r"^===== CASE ", text, flags=re.M)[1:]:
        print(summarize(block))
