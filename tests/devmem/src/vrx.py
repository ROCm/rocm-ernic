#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""vrx: hashing TCP sink for the devmem TX verification pass.

vrx.py PORT [--stall-after BYTES --stall-secs S] [--close-after BYTES [--rst]]
            [--rcvbuf N] [--idle S]
Prints one RESULT line: bytes, sha256 of everything received, whether any run
of 32 zero bytes was seen (the sender's data is random, so that never happens
by chance), and how it ended.
"""

import argparse
import hashlib
import socket
import struct
import sys
import time

ap = argparse.ArgumentParser()
ap.add_argument("port", type=int)
ap.add_argument("--stall-after", type=int, default=0)
ap.add_argument("--stall-secs", type=float, default=0)
ap.add_argument("--stalls", type=int, default=1)
ap.add_argument("--close-after", type=int, default=0)
ap.add_argument("--rst", action="store_true")
ap.add_argument("--rcvbuf", type=int, default=0)
ap.add_argument("--idle", type=float, default=0)
a = ap.parse_args()

ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
if a.rcvbuf:
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, a.rcvbuf)
ls.bind(("0.0.0.0", a.port))
ls.listen(1)
print("vrx: listening", a.port, flush=True)
c, peer = ls.accept()
ls.close()
if a.idle:
    c.settimeout(a.idle)
h = hashlib.sha256()
total = 0
zero_runs = 0
zeros = bytes(32)
end = "eof"
stalls_done = 0
next_stall = a.stall_after
t0 = time.time()
while True:
    try:
        b = c.recv(1 << 20)
    except socket.timeout:
        end = "idle-timeout"
        break
    except OSError as e:
        end = "error:" + str(e)
        break
    if not b:
        break
    h.update(b)
    if zeros in b:
        zero_runs += 1
    total += len(b)
    if a.stall_after and stalls_done < a.stalls and total >= next_stall:
        stalls_done += 1
        next_stall = total + a.stall_after
        print("vrx: stalling reads at", total, "for", a.stall_secs, "s", flush=True)
        time.sleep(a.stall_secs)
    if a.close_after and total >= a.close_after:
        end = "closed-by-receiver" + ("-rst" if a.rst else "")
        if a.rst:
            c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        break
c.close()
dt = time.time() - t0
print(
    f"vrx: RESULT bytes={total} sha256={h.hexdigest()} zero_runs_of_32={zero_runs}"
    f" end={end} secs={dt:.2f} MiB/s={total / max(dt, 1e-9) / 1048576:.1f}",
    flush=True,
)
sys.exit(0)
