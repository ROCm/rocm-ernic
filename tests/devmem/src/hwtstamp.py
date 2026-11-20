#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""SIOCSHWTSTAMP / SIOCGHWTSTAMP on argv[1]: tx_type ON, rx_filter ALL, then OFF/NONE."""

import array
import fcntl
import socket
import struct
import sys

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


def io(cmd, flags, tx, rx):
    """Issue the hwtstamp ioctl @cmd with a struct hwtstamp_config and print the result."""
    cfg = bytearray(struct.pack("iii", flags, tx, rx))
    buf = array.array("B", cfg)
    ifr = struct.pack("16sP", sys.argv[1].encode(), buf.buffer_info()[0])
    try:
        fcntl.ioctl(s, cmd, ifr)
        print(hex(cmd), "ok", struct.unpack("iii", buf.tobytes()))
    except OSError as e:
        print(hex(cmd), "errno", e.errno, e.strerror)


io(0x89B1, 0, 0, 0)
io(0x89B0, 0, 1, 1)
io(0x89B0, 0, 0, 0)
