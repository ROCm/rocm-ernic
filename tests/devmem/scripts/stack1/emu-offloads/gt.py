#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""Guest-side traffic tool for the emulator TX-offload validation.

gt.py rx PORT LOG            TCP (dual-stack) + UDP receiver; one JSON line per
                             TCP connection (bytes, sha256) appended to LOG.
                             Each TCP connection is answered with "bytes sha\n"
                             before close. Each UDP datagram is answered with
                             its sequence number and a hash of what arrived.
gt.py tx  DST PORT SIZE...   one TCP connection per SIZE, payload from a seeded
                             generator; prints one JSON line per connection
                             with the sender's hash, the receiver's reply, and
                             TCP_INFO total_retrans read after the peer's FIN.
gt.py udp DST PORT SIZE...   stop-and-wait datagrams of each SIZE.

Only the Python standard library; runs on 3.10+.
"""

import argparse
import hashlib
import json
import os
import random
import select
import socket
import struct
import sys
import tempfile
import threading
import time

SO_ZEROCOPY = 60
MSG_ZEROCOPY = 0x4000000
SO_EE_ORIGIN_ZEROCOPY = 5
TCP_INFO_LEN = 232


def tcp_info(sock):
    """The TCP_INFO fields of @sock that the evidence records."""
    raw = sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, TCP_INFO_LEN)
    u32 = struct.unpack_from("<24I", raw, 8)
    # dsack_dups: retransmitted segments the peer reported it already had.
    dsack = struct.unpack_from("<I", raw, 216)[0] if len(raw) >= 220 else -1
    return {
        "snd_mss": u32[2],
        "unacked": u32[4],
        "lost": u32[6],
        "pmtu": u32[13],
        "total_retrans": u32[23],
        "dsack_dups": dsack,
    }


def addrinfo(dst, port, kind):
    """The first getaddrinfo() result for @dst and @port."""
    return socket.getaddrinfo(dst, port, type=kind)[0]


# ---------------------------------------------------------------- receiver


def rx_conn(conn, peer, log, lock):
    """Hash what one connection sends, answer with the count and hash, and log it."""
    h = hashlib.sha256()
    n = 0
    try:
        while True:
            b = conn.recv(1 << 20)
            if not b:
                break
            h.update(b)
            n += len(b)
        conn.sendall(f"{n} {h.hexdigest()}\n".encode())
        # Wait (bounded) for the reply to be acknowledged, so this socket's
        # own retransmit count is final when it is logged.
        conn.shutdown(socket.SHUT_WR)
        deadline = time.monotonic() + 3
        info = tcp_info(conn)
        while info["unacked"] and time.monotonic() < deadline:
            time.sleep(0.002)
            info = tcp_info(conn)
    except OSError as e:
        with lock:
            log.write(json.dumps({"error": str(e), "bytes": n}) + "\n")
            log.flush()
        conn.close()
        return
    with lock:
        log.write(
            json.dumps(
                {
                    "bytes": n,
                    "sha": h.hexdigest(),
                    "peer": peer[0],
                    "unacked": info["unacked"],
                    "total_retrans": info["total_retrans"],
                    "dsack_dups": info["dsack_dups"],
                }
            )
            + "\n"
        )
        log.flush()
    conn.close()


def rx_udp(port):
    """Answer every datagram with its first four bytes, its length and its hash."""
    s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
    s.bind(("::", port))
    while True:
        data, peer = s.recvfrom(70000)
        s.sendto(
            data[:4]
            + struct.pack("!I", len(data))
            + hashlib.sha256(data).digest()[:16],
            peer,
        )


def cmd_rx(a):
    """The receiver: TCP and UDP on a.port, one log line per connection."""
    lock = threading.Lock()
    threading.Thread(target=rx_udp, args=(a.port,), daemon=True).start()
    ls = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    ls.bind(("::", a.port))
    ls.listen(64)
    with open(a.log, "a", encoding="ascii") as log:
        while True:
            conn, peer = ls.accept()
            threading.Thread(
                target=rx_conn, args=(conn, peer, log, lock), daemon=True
            ).start()


# ------------------------------------------------------------------ sender


def gen(rng, size, block=1 << 20):
    """@size random bytes from @rng, in blocks."""
    left = size
    while left:
        n = min(left, block)
        yield rng.randbytes(n)
        left -= n


def send_plain(s, rng, size, h, a):
    """Send with sendall(), a.chunk bytes at a time."""
    for b in gen(rng, size, a.chunk or (1 << 18)):
        h.update(b)
        s.sendall(b)


def send_small(s, rng, size, h, a):
    """Many small writes; sizes cycle through a list that crosses MSS."""
    sizes = [int(x) for x in a.pattern.split(",")]
    left, i = size, 0
    while left:
        n = min(left, sizes[i % len(sizes)])
        i += 1
        b = rng.randbytes(n)
        h.update(b)
        s.sendall(b)
        left -= n
    return {"writes": i}


def send_sendfile(s, rng, size, h, a):
    """sendfile() from a file; with --chunk, non-adjacent pieces of that
    size under TCP_CORK so one skb collects many page frags."""
    with tempfile.NamedTemporaryFile(dir=a.filedir) as f:
        for b in gen(rng, size):
            f.write(b)
        f.flush()
        fd = f.fileno()
        if a.chunk:
            c = a.chunk
            slots = list(range(0, size, c))
            order = slots[0::2] + slots[1::2]
        else:
            c = size
            order = [0]
        if a.cork:
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_CORK, 1)
        calls = 0
        for off in order:
            n = min(c, size - off)
            h.update(os.pread(fd, n, off))
            while n:
                k = os.sendfile(s.fileno(), fd, off, n)
                if k == 0:
                    raise OSError("sendfile returned 0")
                off += k
                n -= k
                calls += 1
        if a.cork:
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_CORK, 0)
        return {"sendfile_calls": calls, "pieces": len(order)}


def zc_wait(s, want_hi, st):
    """Read zerocopy notifications until the one covering id want_hi."""
    deadline = time.monotonic() + 30
    while st["hi"] < want_hi:
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError(f"no zerocopy notification for {want_hi}")
        p = select.poll()
        p.register(s, 0)  # POLLERR is always reported
        if not p.poll(left * 1000):
            continue
        try:
            _, ancdata, _, _ = s.recvmsg(
                1, 512, socket.MSG_ERRQUEUE | socket.MSG_DONTWAIT
            )
        except BlockingIOError:
            continue
        for _lvl, _typ, data in ancdata:
            # struct sock_extended_err: errno, origin, type, code, pad, info, data
            ee = struct.unpack_from("<IBBBBII", data)
            if ee[1] != SO_EE_ORIGIN_ZEROCOPY or ee[0] != 0:
                raise OSError(f"errqueue: errno {ee[0]} origin {ee[1]}")
            st["hi"] = max(st["hi"], ee[6])
            st["notes"] += 1
            if ee[3] & 1:
                st["copied"] += ee[6] - ee[5] + 1


def send_zerocopy(s, rng, size, h, a):
    """MSG_ZEROCOPY; the buffer is overwritten as soon as the kernel says
    it is done with it, so a device that read it late corrupts the hash."""
    s.setsockopt(socket.SOL_SOCKET, SO_ZEROCOPY, 1)
    st = {"hi": -1, "notes": 0, "copied": 0}
    sends = 0
    for b in gen(rng, size, a.chunk or (1 << 16)):
        h.update(b)
        buf = bytearray(b)
        mv = memoryview(buf)
        while mv:
            k = s.send(mv, MSG_ZEROCOPY)
            mv = mv[k:]
            sends += 1
        zc_wait(s, sends - 1, st)
        buf[:] = bytes(len(buf))
    return {
        "zc_sends": sends,
        "zc_notifications": st["notes"],
        "zc_copied_sends": st["copied"],
    }


MODES = {
    "plain": send_plain,
    "small": send_small,
    "sendfile": send_sendfile,
    "zerocopy": send_zerocopy,
}


def tx_connect(a):
    """A connected socket with the options the arguments ask for."""
    fam, kind, proto, _, sa = addrinfo(a.dst, a.port, socket.SOCK_STREAM)
    s = socket.socket(fam, kind, proto)
    if a.mss:
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, a.mss)
    if a.nodelay:
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if a.dev:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, a.dev.encode())
    # IPv6 extension headers on every packet of the connection (RFC
    # 3542; needs CAP_NET_RAW): one 8-byte header holding a PadN option.
    ext = b"\x00\x00\x01\x04\x00\x00\x00\x00"
    if a.v6opts in ("hop", "both"):
        s.setsockopt(socket.IPPROTO_IPV6, 54, ext)  # IPV6_HOPOPTS
    if a.v6opts in ("dst", "both"):
        s.setsockopt(socket.IPPROTO_IPV6, 59, ext)  # IPV6_DSTOPTS
    s.settimeout(30)
    s.connect(sa)
    # Blocking from here (sendfile() needs it); a stuck transfer ends
    # with EAGAIN from the kernel timeouts, which is a failure.
    s.settimeout(None)
    tv = struct.pack("ll", 120, 0)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVTIMEO, tv)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDTIMEO, tv)
    return s


def tx_one(a, size):
    """Send @size bytes on a new connection; print the result; whether it matched."""
    s = tx_connect(a)
    rng = random.Random(a.seed * 1000003 + size)
    h = hashlib.sha256()
    t0 = time.monotonic()
    extra = MODES[a.mode](s, rng, size, h, a) or {}
    s.shutdown(socket.SHUT_WR)
    reply = b""
    while not reply.endswith(b"\n"):
        b = s.recv(200)
        if not b:
            break
        reply += b
    while s.recv(200):
        pass
    secs = time.monotonic() - t0
    info = tcp_info(s)
    s.close()
    ok = reply.decode().strip() == f"{size} {h.hexdigest()}"
    out = {
        "mode": a.mode + (f"+v6opts-{a.v6opts}" if a.v6opts else ""),
        "dst": a.dst,
        "bytes": size,
        "sha": h.hexdigest(),
        "rx_reply": reply.decode().strip(),
        "match": ok,
        "secs": round(secs, 3),
        "MiB_s": round(size / 1048576 / secs, 1) if secs else 0,
    }
    out.update(info)
    out.update(extra)
    print(json.dumps(out), flush=True)
    return ok


def cmd_tx(a):
    """The sender: one connection for each size."""
    rc = 0
    for size in a.sizes:
        if not tx_one(a, size):
            rc = 1
    return rc


def csum_fold(total):
    """@total folded to 16 bits."""
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return total


def words(b):
    """The sum of the big-endian 16-bit words of @b."""
    if len(b) & 1:
        b += b"\0"
    return sum(struct.unpack(f"!{len(b) // 2}H", b))


def zero_csum_payload(s, sa, fam, payload):
    """Rewrite the last two bytes so the UDP checksum computes to 0 (which
    must go on the wire as 0xffff)."""
    src = s.getsockname()
    ulen = 8 + len(payload)
    if fam == socket.AF_INET:
        ps = (
            words(socket.inet_pton(fam, src[0]) + socket.inet_pton(fam, sa[0]))
            + 17
            + ulen
        )
    else:
        ps = (
            words(
                socket.inet_pton(fam, src[0].split("%")[0])
                + socket.inet_pton(fam, sa[0].split("%")[0])
            )
            + 17
            + ulen
        )
    part = csum_fold(ps + src[1] + sa[1] + ulen + words(payload[:-2]))
    last = (0xFFFF - part) & 0xFFFF
    return payload[:-2] + struct.pack("!H", last)


def udp_echo(s, data, size):
    """Send @data and wait for its answer.

    Returns (good, lost or bad; when it was sent; the RTT or None; a detail
    for the report or None).
    """
    t0 = time.monotonic()
    try:
        s.send(data)
    except OSError as e:
        return "bad", t0, None, {"send_error": str(e)}
    want = data[:4] + struct.pack("!I", len(data)) + hashlib.sha256(data).digest()[:16]
    try:
        while True:
            r = s.recv(100)
            if r[:4] == data[:4] or size < 4:
                break
    except socket.timeout:
        return "lost", t0, None, {"lost": True}
    rtt = time.monotonic() - t0
    if r == want:
        return "good", t0, rtt, None
    return "bad", t0, rtt, {"corrupt": True}


def udp_socket(a):
    """A connected UDP socket, its peer address and its family."""
    fam, kind, proto, _, sa = addrinfo(a.dst, a.port, socket.SOCK_DGRAM)
    s = socket.socket(fam, kind, proto)
    if a.dev:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, a.dev.encode())
    s.connect(sa)
    s.settimeout(3)
    return s, sa, fam


def rtt_stats(rtts, spikes):
    """The round-trip time summary of the report."""
    rtts.sort()
    return {
        "p50": round(rtts[len(rtts) // 2] * 1e3, 3),
        "p99": round(rtts[len(rtts) * 99 // 100] * 1e3, 3),
        "max": round(rtts[-1] * 1e3, 3),
        "over_3ms": sum(1 for x in rtts if x > 0.003),
        "spikes_at_s": spikes[:40],
    }


def cmd_udp(a):
    """Stop-and-wait UDP echo of every size, a.repeat times; prints one JSON line."""
    s, sa, fam = udp_socket(a)
    rng = random.Random(a.seed)
    n = {"sent": 0, "good": 0, "lost": 0, "bad": 0}
    rtts, spikes, detail = [], [], []
    start = time.monotonic()
    for size in a.sizes * a.repeat:
        data = struct.pack("!I", n["sent"]) + rng.randbytes(max(size - 4, 0))
        data = data[:size] if size < 4 else data
        if a.zero_csum and size >= 8 and size % 2 == 0:
            data = zero_csum_payload(s, sa, fam, data)
        res = udp_echo(s, data, size)
        if res[3]:
            detail.append({"seq": n["sent"], "size": size, **res[3]})
        n["sent"] += 1
        n[res[0]] += 1
        if res[2] is not None:
            rtts.append(res[2])
            if res[2] > 0.003:
                spikes.append((round(res[1] - start, 3), round(res[2] * 1e3, 1)))
    out = {
        "mode": "udp",
        "dst": a.dst,
        **n,
        "zero_csum": a.zero_csum,
        "match": n["good"] == n["sent"],
        "detail": detail[:20],
    }
    if rtts:
        out["rtt_ms"] = rtt_stats(rtts, spikes)
    print(json.dumps(out), flush=True)
    return 0 if n["good"] == n["sent"] else 1


def main():
    """Parse the command line and run the command."""
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("rx")
    r.add_argument("port", type=int)
    r.add_argument("log")
    for name in ("tx", "udp"):
        t = sub.add_parser(name)
        t.add_argument("dst")
        t.add_argument("port", type=int)
        t.add_argument("sizes", type=int, nargs="+")
        t.add_argument("--seed", type=int, default=1)
        t.add_argument("--dev")
        if name == "tx":
            t.add_argument("--mode", choices=sorted(MODES), default="plain")
            t.add_argument("--chunk", type=int, default=0)
            t.add_argument("--pattern", default="1,7,100,1448,3000,13")
            t.add_argument("--nodelay", action="store_true")
            t.add_argument("--cork", action="store_true")
            t.add_argument("--mss", type=int, default=0)
            t.add_argument("--filedir", default="/tmp")
            t.add_argument("--v6opts", choices=("hop", "dst", "both"))
        else:
            t.add_argument("--repeat", type=int, default=1)
            t.add_argument("--zero-csum", action="store_true")
    a = p.parse_args()
    return {"rx": cmd_rx, "tx": cmd_tx, "udp": cmd_udp}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
