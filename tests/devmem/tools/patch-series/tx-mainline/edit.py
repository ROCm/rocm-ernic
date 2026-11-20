#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""Apply exact-match replacements to a file: edit.py FILE < spec
spec: blocks separated by lines '@@@@'; each block is OLD, a line '====', NEW."""

import sys
from pathlib import Path

path = Path(sys.argv[1])
src = path.read_text(encoding="utf-8")
spec = sys.stdin.read()
for n, block in enumerate(spec.split("\n@@@@\n")):
    old, new = block.split("\n====\n")
    old = old.strip("\n") + "\n"
    new = new.strip("\n") + "\n" if new.strip("\n") else ""
    cnt = src.count(old)
    if cnt != 1:
        sys.exit(f"{path}: block {n}: {cnt} matches for:\n{old}")
    src = src.replace(old, new)
path.write_text(src, encoding="utf-8")
