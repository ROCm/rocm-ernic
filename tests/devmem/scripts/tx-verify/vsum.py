#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""One line per vcase in the given evidence files."""

import re
import sys
from pathlib import Path


def field(case, pattern, default="-"):
    """The first group of @pattern in the text of @case, or @default."""
    m = re.search(pattern, case)
    return m.group(1) if m else default


def summarize(c):
    """The summary line of one case, whose text after the CASE marker is @c."""
    ur = sum(int(x) for x in re.findall(r"@unreadable_skbs\[\d+, \d+\]: (\d+)", c))
    rd = sum(int(x) for x in re.findall(r"@readable_skbs\[[1-9]\d*, \d+\]: (\d+)", c))
    nb = re.findall(r"@frag_netmem_lowbit\[\d+, (\d)\]: (\d+)", c)
    fr = re.findall(
        r"^\[(\d+)\] +(\d+) \|",
        (
            c[c.find("@unreadable_frags") : c.find("@unreadable_gso")]
            if "@unreadable_frags" in c
            else ""
        ),
        re.M,
    )
    mx = max((int(a) for a, b in fr if int(b)), default="-")
    verdict = field(c, r"=> (DATA-\S+ \S+)", field(c, r"VERDICT.*(NO RECEIVER RESULT)"))
    lowbit = ",".join(f"{a}:{b}" for a, b in nb) or "-"
    drop = field(c, r"eth\.tx_unreadable_drop\s+\d+ -> \d+\s+\((\S+)\)", "+0")
    txdrop = field(c, r"netdev\.tx_dropped\s+\d+ -> \d+\s+\((\S+)\)", "+0")
    retrans = field(c, r"tcp\.TcpRetransSegs\s+\d+ -> \d+\s+\((\S+)\)", "+0")
    splat = "/".join(re.findall(r"\((\d+) splat lines\)", c))
    rx = field(c, r"receiver-got=(\d+)")
    tx = field(c, r"sender-accepted=(\d+)")
    other = field(c, r"other=(-?\d+)")
    return (
        f"{c.split()[0]:<58} rx={rx:<11} tx={tx:<11} {verdict:<26} "
        f"unreadable_skbs={ur:<7} readable={rd:<3} maxfrags={mx:<3} "
        f"lowbit={lowbit} drop={drop} txdrop={txdrop} retrans={retrans} "
        f"dmaOther={other} splat={splat}"
    )


for f in sys.argv[1:]:
    t = Path(f).read_text(errors="replace", encoding="utf-8")
    for text in t.split("===== CASE ")[1:]:
        print(summarize(text))
