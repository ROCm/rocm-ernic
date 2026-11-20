#!/bin/bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# pools.sh: dump page pools via netlink; flag orphans (pools with no ifindex).
Y=/mnt/hostfs/linux-rx/tools/net/ynl
out=$(python3 $Y/pyynl/cli.py --spec /mnt/hostfs/linux-rx/Documentation/netlink/specs/netdev.yaml --dump page-pool-get --output-json 2>&1)
echo "$out" | python3 -c '
import json,sys
p=json.load(sys.stdin)
orph=[x for x in p if "ifindex" not in x]
print("pools:", len(p), "orphans(no ifindex):", len(orph), "detached:", len([x for x in p if "detach-time" in x]))
for x in p:
    if "ifindex" not in x or x.get("ifindex",0)!=2: print("  ", x)
'
