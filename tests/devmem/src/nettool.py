#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""Small traffic tool for the HDS tests (no dependencies).
  nettool.py tcp-recv ADDR PORT            accept one connection, print "bytes sha256"
  nettool.py tcp-send ADDR PORT SIZE MODE [SPORT]
                                           MODE = bulk | small (1..5 byte TCP_NODELAY writes)
  nettool.py udp-recv ADDR PORT SECONDS    verify self-checking datagrams, print good/bad/by-size
  nettool.py udp-send ADDR PORT COUNT SIZE[,SIZE...]
ADDR may be IPv4, or IPv6 with %scope. Payload is a deterministic pattern."""

import hashlib
import socket
import sys
import time


def pattern(n, seed=b"hds"):
    """@n bytes of a deterministic pattern derived from @seed."""
    out = bytearray()
    i = 0
    while len(out) < n:
        out += hashlib.sha256(seed + i.to_bytes(8, "big")).digest()
        i += 1
    return bytes(out[:n])


def addrinfo(addr, port, typ):
    """The address family, socket type and sockaddr of @addr and @port."""
    fam, typ, _, _, sa = socket.getaddrinfo(addr, port, 0, typ)[0]
    return fam, typ, sa


def tcp_recv(addr, port):
    """Accept one connection and print the byte count and SHA-256 of what it sends."""
    fam, typ, sa = addrinfo(addr, port, socket.SOCK_STREAM)
    s = socket.socket(fam, typ)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(sa)
    s.listen(1)
    print("listening", flush=True)
    c, _ = s.accept()
    h = hashlib.sha256()
    n = 0
    while True:
        b = c.recv(1 << 16)
        if not b:
            break
        h.update(b)
        n += len(b)
    print(n, h.hexdigest(), flush=True)


def tcp_send(addr, port, size, mode, sport=0):
    """Send @size pattern bytes, in one go or in tiny writes, and print their hash."""
    fam, typ, sa = addrinfo(addr, port, socket.SOCK_STREAM)
    s = socket.socket(fam, typ)
    if sport:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("", sport))
    s.connect(sa)
    data = pattern(size)
    if mode == "small":
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        off = 0
        k = 0
        while off < size:
            n = 1 + k % 5
            s.send(data[off : off + n])
            off += n
            k += 1
            time.sleep(0.0005)
    else:
        s.sendall(data)
    s.close()
    print(size, hashlib.sha256(data).hexdigest(), flush=True)


def udp_recv(addr, port, secs):
    """Count the valid and invalid datagrams that arrive in @secs seconds."""
    fam, typ, sa = addrinfo(addr, port, socket.SOCK_DGRAM)
    s = socket.socket(fam, typ)
    s.bind(sa)
    s.settimeout(0.5)
    print("listening", flush=True)
    good = bad = 0
    sizes = {}
    end = time.time() + secs
    while time.time() < end:
        try:
            b, _ = s.recvfrom(65536)
        except socket.timeout:
            continue
        ok = len(b) >= 1 and (
            len(b) < 33
            and b == pattern(len(b), b"u")
            or len(b) >= 33
            and b[:32] == hashlib.sha256(b[32:]).digest()
        )
        good += ok
        bad += not ok
        sizes[len(b)] = sizes.get(len(b), 0) + 1
    print("good", good, "bad", bad, "by-size", sorted(sizes.items()), flush=True)


def udp_send(addr, port, count, sizes):
    """Send @count rounds of one self-checking datagram of each size in @sizes."""
    fam, typ, sa = addrinfo(addr, port, socket.SOCK_DGRAM)
    s = socket.socket(fam, typ)
    sent = 0
    for i in range(count):
        for n in sizes:
            if n < 33:
                b = pattern(n, b"u")
            else:
                body = pattern(n - 32, i.to_bytes(4, "big"))
                b = hashlib.sha256(body).digest() + body
            s.sendto(b, sa)
            sent += 1
            time.sleep(0.001)
    print("sent", sent, flush=True)


a = sys.argv
if a[1] == "tcp-recv":
    tcp_recv(a[2], int(a[3]))
elif a[1] == "tcp-send":
    tcp_send(a[2], int(a[3]), int(a[4]), a[5], int(a[6]) if len(a) > 6 else 0)
elif a[1] == "udp-recv":
    udp_recv(a[2], int(a[3]), float(a[4]))
elif a[1] == "udp-send":
    udp_send(a[2], int(a[3]), int(a[4]), [int(x) for x in a[5].split(",")])
