#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""Expected bytes of the devmem sender's stream, for a host-side check.

plan_bytes.py [-x] SEED PSIZE OFF LEN CHUNK GAP ADV STREAM_POS NBYTES

Prints the sha256 of NBYTES of the sent byte stream starting at
STREAM_POS, or with -x the bytes themselves on stdout.  The plan
arithmetic is the sender's plan_start()/plan_iov() and the byte pattern
is its pat(), written again here so that the check does not depend on
the program it is checking.

case.sh uses it to hash the bytes a held receive fragment should hold,
and compares that with the receiving NIC server's own mapping of the
GPU's BAR.
"""

import hashlib
import sys

MASK64 = (1 << 64) - 1


def pat(x, seed):
    """The sender's position-dependent byte at stream offset x."""
    w = ((x >> 3) + seed * 0x100000001B3) & MASK64
    w ^= w >> 30
    w = (w * 0xBF58476D1CE4E5B9) & MASK64
    w ^= w >> 27
    w = (w * 0x94D049BB133111EB) & MASK64
    w ^= w >> 31
    return (w >> ((x & 7) * 8)) & 0xFF


class Plan:
    """One send plan: which buffer bytes each iovec of each send covers."""

    def __init__(self, seed, psize, off, length, chunk, gap, adv):
        # pylint: disable=too-many-arguments,too-many-positional-arguments
        self.seed = seed
        self.length = length
        self.chunk = chunk or length
        self.step = self.chunk + gap
        self.first, self.advance = off, adv
        last = (self.iovcnt - 1) * self.chunk
        span = (self.iovcnt - 1) * self.step + (length - last)
        self.wrap = psize - span + 1

    @property
    def iovcnt(self):
        """Iovecs per send."""
        return (self.length + self.chunk - 1) // self.chunk

    def iov(self, i, j):
        """Buffer offset and length of iovec j of send i."""
        start = (self.first + i * self.advance) % self.wrap
        return start + j * self.step, min(self.chunk, self.length - j * self.chunk)

    def bytes_at(self, pos, count):
        """The count bytes of the stream starting at stream offset pos."""
        i = pos // self.length
        j = 0
        rest = pos % self.length
        while rest:
            _, iov_len = self.iov(i, j)
            if rest < iov_len:
                break
            rest -= iov_len
            j += 1
        out = bytearray()
        while len(out) < count:
            base, iov_len = self.iov(i, j)
            take = min(iov_len - rest, count - len(out))
            for t in range(take):
                out.append(pat(base + rest + t, self.seed))
            rest += take
            if rest == iov_len:
                rest = 0
                j += 1
                if j == self.iovcnt:
                    j = 0
                    i += 1
        return bytes(out)


def main():
    """Parse the plan from argv and print the hash, or the bytes."""
    raw = "-x" in sys.argv
    args = [v for v in sys.argv[1:] if v != "-x"]
    if len(args) != 9:
        print(__doc__.splitlines()[2], file=sys.stderr)
        return 2
    seed, psize, off, length, chunk, gap, adv, pos, count = (int(v, 0) for v in args)
    out = Plan(seed, psize, off, length, chunk, gap, adv).bytes_at(pos, count)
    if raw:
        sys.stdout.buffer.write(out)
    else:
        print(hashlib.sha256(out).hexdigest())
    return 0


if __name__ == "__main__":
    sys.exit(main())
