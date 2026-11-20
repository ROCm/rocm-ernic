#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""Print every kernel log line an evidence file recorded after a case's marker
(the '--- dmesg guest N since marker' sections), with the case name."""

import re
import sys
from pathlib import Path

for path in sys.argv[1:]:
    case = "-"
    guest = None
    for line in (
        Path(path).read_text(errors="replace", encoding="utf-8").splitlines(True)
    ):
        m = re.match(r"===== CASE (\S+)", line)
        if m:
            case, guest = m.group(1), None
            continue
        m = re.match(r"--- dmesg guest (\d)", line)
        if m:
            guest = m.group(1)
            continue
        if guest and re.match(r"\[ *\d+\.\d+\]", line):
            print(f"{path.split('/')[-1]} {case} g{guest}: {line.rstrip()[:200]}")
        elif guest and not line.startswith("["):
            guest = None
