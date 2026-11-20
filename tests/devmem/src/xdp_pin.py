#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""Load a 2-instruction XDP_PASS program and pin it at argv[1] (no clang in the guest)."""

import ctypes
import os
import struct
import sys

libc = ctypes.CDLL(None, use_errno=True)


def bpf(cmd, attr_bytes):
    """bpf(2) with the attributes in @attr_bytes; returns its result or raises OSError."""
    buf = ctypes.create_string_buffer(attr_bytes, 128)
    r = libc.syscall(321, cmd, buf, 128)
    if r < 0:
        e = ctypes.get_errno()
        raise OSError(e, os.strerror(e))
    return r


insns = struct.pack("<BBhi", 0xB7, 0, 0, 2) + struct.pack("<BBhi", 0x95, 0, 0, 0)
ib = ctypes.create_string_buffer(insns)
lic = ctypes.create_string_buffer(b"GPL")
attr = struct.pack(
    "<IIQQIIQII16s",
    6,
    2,
    ctypes.addressof(ib),
    ctypes.addressof(lic),
    0,
    0,
    0,
    0,
    0,
    b"xdp_pass",
)
fd = bpf(5, attr)
path = ctypes.create_string_buffer(sys.argv[1].encode())
bpf(6, struct.pack("<QI", ctypes.addressof(path), fd))
print("pinned", sys.argv[1])
