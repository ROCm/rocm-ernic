#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# cwr-check.sh CAPTURE.txt.gz...: for each decoded capture, count CE-marked
# frames and print each TCP data segment carrying CWR with its neighbours
# (time, tos/class, flags, seq, payload): CWR must be on the first segment of
# a TSO burst only.
for f in "$@"; do echo "== $f"; zcat "$f" | python3 -c '
import sys,re
recs=[];cur=None
for l in sys.stdin:
    l=l.rstrip("\n")
    if l[:1] in " \t" and cur is not None: cur+=" "+l.strip()
    else:
        if cur: recs.append(cur)
        cur=l
recs.append(cur)
ce=sum(1 for r in recs if re.search(r"tos 0x3,|class 0x03,",r)); print("frames with CE:",ce,"of",len(recs))
for i,r in enumerate(recs):
    m=re.search(r"Flags \[([^\]]*)\].* length (\d+)$",r)
    if m and "W" in m.group(1) and int(m.group(2))>0:
        for j in range(i-1,min(i+4,len(recs))):
            mm=re.search(r"^(\S+) .*?(tos 0x\w+|class 0x\w+).*Flags \[([^\]]*)\].*seq (\S+) .* length (\d+)$",recs[j]); print("  ",mm.groups() if mm else recs[j][:150])
        print()
'; done
