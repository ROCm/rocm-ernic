#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""Send UDP datagrams to DST:7777 from port 7778 for SECS seconds and check
the echoes an XDP program on DST reflects. Payload = 8-byte sequence number,
then bytes (i*131 + 7 + seq*29) & 0xff, so every datagram differs from its
neighbours (FIXED=1 in the environment: the same bytes in every datagram, as
the first runs of this test used). Prints sent / echoed / bad counts.
Usage: udp_echo_check.py DST SECS SIZE [PPS]"""

import os
import socket
import struct
import sys
import time

dst, secs, size = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])
pps = float(sys.argv[4]) if len(sys.argv) > 4 else 500.0
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("", 7778))
s.setblocking(False)
fixed = os.environ.get("FIXED") == "1"
base = bytes((i * 131 + 7) & 0xFF for i in range(size))


def body(seq):
    """The payload after the sequence number of datagram @seq."""
    if fixed:
        return base[8:]
    return bytes((b + seq * 29) & 0xFF for b in base[8:])


count = {"sent": 0, "echoed": 0, "bad": 0}
seen = set()
end = time.monotonic() + secs
nxt = time.monotonic()


def drain():
    """Read every echo waiting on the socket, counting the good and the bad."""
    while True:
        try:
            dgram, addr = s.recvfrom(65536)
        except BlockingIOError:
            return
        if addr[1] != 7777 or len(dgram) != size:
            count["bad"] += 1
            continue
        (seq,) = struct.unpack("!Q", dgram[:8])
        if seq in seen or seq >= count["sent"] or dgram[8:] != body(seq):
            count["bad"] += 1
            continue
        seen.add(seq)
        count["echoed"] += 1


while time.monotonic() < end:
    try:
        s.sendto(struct.pack("!Q", count["sent"]) + body(count["sent"]), (dst, 7777))
        count["sent"] += 1
    except BlockingIOError:
        pass
    drain()
    nxt += 1.0 / pps
    d = nxt - time.monotonic()
    if d > 0:
        time.sleep(d)
t = time.monotonic() + 1.0
while time.monotonic() < t:
    drain()
    time.sleep(0.01)
print(
    f"UDP-ECHO {'fixed' if fixed else 'per-datagram'} pattern size={size}"
    f" sent={count['sent']} echoed={count['echoed']} bad={count['bad']}"
    f" lost={count['sent'] - count['echoed']}"
)
