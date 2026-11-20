# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""Set single bytes in tests/golden/ionic_compat_identity.h: name:index:value ..."""

import re
import sys
from pathlib import Path

p = Path(sys.argv[1])
s = p.read_text(encoding="utf-8")
for spec in sys.argv[2:]:
    name, idx, val = spec.split(":")
    idx = int(idx)
    val = int(val, 16)
    m = re.search(r"static const uint8_t %s\[\d+\] = \{" % name, s)
    toks = list(re.finditer(r"0x[0-9a-f]{2}", s[m.end() :]))
    t = toks[idx]
    a = m.end() + t.start()
    print(name, idx, s[a : a + 4], "->", f"0x{val:02x}")
    s = s[:a] + f"0x{val:02x}" + s[a + 4 :]
p.write_text(s, encoding="utf-8")
