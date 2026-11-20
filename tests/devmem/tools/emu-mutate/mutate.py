# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""Mutation check of the emulator's Tx offload code.

For each mutant in M, build tests/test_ionic_eth_tx against a copy of one
source file with one check removed or weakened, and run it: a mutant the
unit test does not fail is a check nothing tests.  Nothing in the
repository or its build directory is written; the mutated sources, their
objects and the test binary go to OUT.

Usage: mutate.py [BUILD [OUT [MUTANT...]]]

  BUILD   a configured Ninja build directory of the repository at REPO
          (default REPO/build-gcc); a sanitizer build runs the mutants
          under that sanitizer
  OUT     scratch directory (default $DEVMEM_ROOT/test/emu-mutate/out;
          one of the two must be given)
  MUTANT  names from M to run instead of all of them

REPO is $ERNIC_REPO, or the repository this file is in.
"""

import os
import shlex
import subprocess
import sys
from pathlib import Path

REPO = os.environ.get("ERNIC_REPO") or str(Path(__file__).resolve().parents[4])
BUILD = sys.argv[1] if len(sys.argv) > 1 else REPO + "/build-gcc"
if len(sys.argv) > 2:
    OUT = sys.argv[2]
elif os.environ.get("DEVMEM_ROOT"):
    OUT = os.environ["DEVMEM_ROOT"] + "/test/emu-mutate/out"
else:
    sys.exit("mutate.py: give OUT, or set DEVMEM_ROOT to the work area")
os.makedirs(OUT, exist_ok=True)
cmds = subprocess.run(
    ["ninja", "-C", BUILD, "-t", "commands", "tests/test_ionic_eth_tx"],
    capture_output=True,
    text=True,
    check=True,
).stdout.splitlines()
EMU, TX = "src/ionic/ionic_eth_emu.c", "src/ionic/ionic_eth_tx.c"
M = [
    (
        "sg wrap check",
        EMU,
        "q->sg_ring_base > UINT64_MAX - sg_off ||\n            ",
        "",
    ),
    ("sg ring 0", EMU, " && sg_ring_base != 0,", ","),
    ("hdr max removed", EMU, "mss == 0 || hdr_len > TXQ_TSO_HDR_MAX ||", "mss == 0 ||"),
    ("hdr max 252", EMU, "hdr_len > TXQ_TSO_HDR_MAX ||", "hdr_len > 252 ||"),
    (
        "seg0<=hdr removed",
        EMU,
        "seg0 <= hdr_len || !ionic_eth_tso_parse",
        "!ionic_eth_tso_parse",
    ),
    (
        "seg0<hdr",
        EMU,
        "seg0 <= hdr_len || !ionic_eth_tso_parse",
        "seg0 < hdr_len || !ionic_eth_tso_parse",
    ),
    ("eot==last removed", EMU, "&& eot == last;", ";"),
    (
        "check-pass bound 66000",
        EMU,
        "bytes > TXQ_TSO_PAYLOAD_MAX - payload",
        "bytes > 66000U - payload",
    ),
    (
        "send-pass bound removed",
        EMU,
        "if (len > TXQ_TSO_PAYLOAD_MAX - payload_off) {",
        "if (false) {",
    ),
    (
        "first limit",
        EMU,
        "first, 0, (size_t)hdr_len + mss, why);",
        "first, 0, sizeof(emu->frame), why);",
    ),
    (
        "follows ENCAP",
        EMU,
        "(d->flags & (TXQ_DESC_FLAG_VLAN | TXQ_DESC_FLAG_ENCAP |\n"
        "                        TXQ_DESC_FLAG_TSO_SOT)) == 0",
        "(d->flags & (TXQ_DESC_FLAG_VLAN | TXQ_DESC_FLAG_TSO_SOT)) == 0",
    ),
    (
        "ipv4 version",
        TX,
        "ip[IP_VERSION_IHL_OFF] >> IP_VERSION_SHIFT != IP_VERSION ||",
        "",
    ),
    (
        "ipv6 version",
        TX,
        " ||\n        hdr[l3_off] >> IP6_VERSION_SHIFT != IP6_VERSION",
        "",
    ),
    (
        "qinq",
        TX,
        "ethertype == ETH_ETHERTYPE_VLAN || ethertype == ETH_ETHERTYPE_QINQ",
        "ethertype == ETH_ETHERTYPE_VLAN",
    ),
    (
        "no routing",
        TX,
        "next != IP6_NEXT_HDR_HOP_BY_HOP && next != IP6_NEXT_HDR_ROUTING &&",
        "next != IP6_NEXT_HDR_HOP_BY_HOP &&",
    ),
    (
        "no dstopts",
        TX,
        "next != IP6_NEXT_HDR_ROUTING &&\n            next != IP6_NEXT_HDR_DST_OPTS",
        "next != IP6_NEXT_HDR_ROUTING",
    ),
    ("segs left", TX, "ext[IP6_ROUTING_SEGS_LEFT_OFF] != 0", "false"),
    ("RST", TX, "(TCP_FLAG_SYN | TCP_FLAG_RST)", "TCP_FLAG_SYN"),
    ("frag mask", TX, "& IP_FRAGMENT_MASK", "& 0x2000"),
]


def compile_mutant(name, path, msrc, mobj):
    """Compile @msrc, the mutated copy of @path, to @mobj as the build would."""
    for c in cmds:
        if c.endswith("/" + path) or (" -c " in c and c.split()[-1].endswith(path)):
            a = shlex.split(c)
            a[a.index("-o") + 1] = mobj
            a[-1] = msrc
            a = [x for x in a if not x.startswith("-MF") and x not in ("-MD", "-MT")]
            # drop the -MT/-MF operands
            a = [
                x
                for x in a
                if not x.endswith(".o.d") and "CMakeFiles" not in x or x == mobj
            ]
            r = subprocess.run(
                a + ["-Wno-error"],
                cwd=BUILD,
                capture_output=True,
                text=True,
                check=False,
            )
            if r.returncode:
                print(name, "COMPILE FAILED", r.stderr[-600:])
                return False
    return True


def run(name, path, old, new):
    """Build and run the unit test with @old replaced by @new in @path."""
    src = Path(REPO, path).read_text(encoding="utf-8")
    if old is not None:
        if src.count(old) != 1:
            print(f"{name}: SKIPPED, {src.count(old)} matches for its anchor in {path}")
            return
        src = src.replace(old, new)
    base = os.path.basename(path)
    msrc = os.path.join(OUT, base)
    Path(msrc).write_text(src, encoding="utf-8")
    mobj = os.path.join(OUT, base + ".o")
    if not compile_mutant(name, path, msrc, mobj):
        return
    link = [c for c in cmds if " -o tests/test_ionic_eth_tx " in c][-1]
    a = shlex.split(link.split("&&")[1])
    a = [mobj if x.endswith("/" + path + ".o") else x for x in a]
    a[a.index("-o") + 1] = os.path.join(OUT, "t")
    subprocess.run(a, cwd=BUILD, check=True)
    r = subprocess.run(
        [os.path.join(OUT, "t")],
        capture_output=True,
        text=True,
        errors="replace",
        check=False,
    )
    lines = r.stdout.splitlines()
    fails = [l for l in lines if l.startswith("FAIL")]
    rows = sum(l.startswith(("ok", "FAIL")) for l in lines)
    print(f"{name}: rc={r.returncode} rows={rows} fail={len(fails)}")
    for l in fails[:4]:
        print("    ", l[:110])
    if r.returncode not in (0, 1):
        print("    ", r.stderr.strip().splitlines()[:2])


run("unmutated", EMU, None, None)
for m in M:
    if len(sys.argv) <= 3 or m[0] in sys.argv[3:]:
        run(*m)
