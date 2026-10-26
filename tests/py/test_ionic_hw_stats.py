#!/usr/bin/env python3
"""Drive the hardware statistics behind a guest's RDMA hw_counters.

A guest's ``/sys/class/infiniband/<dev>/ports/1/hw_counters`` is built
from four admin commands: STATS_HDRS and QP_STATS_HDRS hand back a table
naming the counters, and STATS_VALS and QP_STATS_VALS fill a buffer of
values the table describes. The driver trusts that table completely --
it takes the names, the value types and the byte offsets from it and
publishes whatever it finds -- so a table that describes itself wrongly
shows up in sysfs as garbage, not as an error.

Nothing here hardcodes the counter set. The structural checks run over
whatever the device advertises, so a counter added to either table is
covered the day it lands. Only the checks that tie a value to traffic
name counters, and they name ones the device has always had.

The buffers are filled with a guard byte first, so a device that writes
past the length the command gave it is caught rather than hidden by a
zeroed page.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import collections
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Imported once tests/py is on sys.path: ctest runs this under python3 -I,
# which leaves the script's directory off it.
# pylint: disable=wrong-import-position
from ernic import ionic
from ernic.check import Suite
from ernic.server import run_vfu_suite

# The fixture is the same pair of connected QPs the driver suite builds,
# and keeping one copy of it means a change to bring-up cannot leave the
# two suites disagreeing about what a working device looks like.
from test_ionic_driver import (
    MR_B,
    MR_LEN,
    QP_A,
    QP_B,
    VA_A,
    VA_B,
    Fixture,
    rdma_a_to_b,
    send_a_to_b,
)

# pylint: enable=wrong-import-position

# struct ionic_v1_stat: a big-endian type_off, then a NUL-terminated name.
STAT_HDR_SIZE = 32
STAT_NAME_OFF = 4
STAT_NAME_LEN = STAT_HDR_SIZE - STAT_NAME_OFF
STAT_TYPE_SHIFT = 28
STAT_OFF_MASK = (1 << STAT_TYPE_SHIFT) - 1
STAT_TYPE_NONE = 0
STAT_TYPE_LE64 = 4
STAT_VAL_SIZE = 8

# What the driver hands these commands: one kzalloc'd page.
BUF_LEN = 4096

# Anything the device leaves alone still reads as this.
GUARD = 0xAA

# A QP id nothing creates. The fixture's QPs are well below it.
UNKNOWN_QP = 0x00FF_0000

Stat = collections.namedtuple("Stat", "name raw type off")


def stats_body(dma, length, id_ver=0):
    """struct ionic_v1_admin_stats_cmd: a DMA target, its size, and an id.

    17 bytes, which is what the driver puts in the WQE header's length.
    """
    return struct.pack("<QIIx", dma, length, id_ver)


def guard_fill(mem, buf, length=BUF_LEN):
    """Poison @length bytes at @buf so any write to them shows up."""
    mem.write(buf, bytes([GUARD]) * length)


def guard_tail(mem, buf, written, length=BUF_LEN):
    """Whether everything past @written bytes of @buf is still poison."""
    tail = mem.read(buf + written, length - written)
    return tail == bytes([GUARD]) * len(tail)


def parse_table(mem, buf, limit=BUF_LEN):
    """Read the ionic_v1_stat table at @buf, stopping at the NONE entry.

    Returns the entries before the terminator. A table that never
    terminates within @limit returns what it found, which the caller
    checks against the number of entries it expected to fit.
    """
    stats = []
    for off in range(0, limit - STAT_HDR_SIZE + 1, STAT_HDR_SIZE):
        entry = mem.read(buf + off, STAT_HDR_SIZE)
        type_off = struct.unpack_from(">I", entry, 0)[0]
        if (type_off >> STAT_TYPE_SHIFT) == STAT_TYPE_NONE:
            break
        raw = entry[STAT_NAME_OFF:]
        stats.append(
            Stat(
                name=raw.split(b"\0", 1)[0].decode("ascii", "replace"),
                raw=raw,
                type=type_off >> STAT_TYPE_SHIFT,
                off=type_off & STAT_OFF_MASK,
            )
        )
    return stats


def read_table(suite, fix, op, id_ver=0):
    """Run a *_STATS_HDRS command and return the table it wrote."""
    buf = fix.stats_buf
    # Zeroed, as the driver's kzalloc leaves it: the terminator is only
    # as good as the buffer behind it.
    fix.mem.write(buf, b"\0" * BUF_LEN)
    status, _ = fix.driver.admin(op, stats_body(buf, BUF_LEN, id_ver))
    suite.check_eq(status, 0, f"STATS_HDRS op {op} completes")
    return parse_table(fix.mem, buf)


def read_values(suite, fix, op, count, id_ver=0):
    """Run a *_STATS_VALS command and return @count values.

    Also checks that the device wrote no more than the @count values its
    own table described, which is the overrun the guard byte is for.
    """
    buf = fix.stats_buf
    guard_fill(fix.mem, buf)
    status, _ = fix.driver.admin(op, stats_body(buf, BUF_LEN, id_ver))
    suite.check_eq(status, 0, f"STATS_VALS op {op} completes")
    written = count * STAT_VAL_SIZE
    suite.check(
        guard_tail(fix.mem, buf, written),
        f"op {op} wrote only the {count} values its table names",
    )
    raw = fix.mem.read(buf, written)
    return [struct.unpack_from("<Q", raw, i * STAT_VAL_SIZE)[0] for i in range(count)]


def named_values(stats, values):
    """Zip a table and its values into {name: value}."""
    return {
        stat.name: values[stat.off // STAT_VAL_SIZE]
        for stat in stats
        if stat.off // STAT_VAL_SIZE < len(values)
    }


def check_table(suite, stats, what):
    """Every structural promise a stats table makes to the driver."""
    suite.check(len(stats) > 0, f"the {what} table names at least one counter")

    suite.check(
        all(stat.type == STAT_TYPE_LE64 for stat in stats),
        f"every {what} counter is an LE64",
    )
    suite.check(
        all(stat.off % STAT_VAL_SIZE == 0 for stat in stats),
        f"every {what} offset is 8-byte aligned, as the driver requires",
    )
    offsets = [stat.off for stat in stats]
    suite.check_eq(
        len(set(offsets)), len(offsets), f"no two {what} counters share an offset"
    )
    suite.check(
        all(off + STAT_VAL_SIZE <= len(stats) * STAT_VAL_SIZE for off in offsets),
        f"every {what} offset lies inside the value area it describes",
    )

    names = [stat.name for stat in stats]
    suite.check_eq(len(set(names)), len(names), f"{what} counter names are unique")
    suite.check(all(names), f"no {what} counter name is empty")
    # The driver writes name[27] = 0 itself, so a name filling the field
    # reaches sysfs truncated. Terminating inside the field is the
    # device's job, and so is zeroing the rest of it: a stray byte after
    # the terminator is wire data the device did not mean to send.
    suite.check(
        all(
            stat.raw == stat.name.encode("ascii", "replace").ljust(STAT_NAME_LEN, b"\0")
            for stat in stats
        ),
        f"every {what} name is NUL-terminated and zero-padded to "
        f"{STAT_NAME_LEN} bytes",
    )
    # A sysfs attribute name, and a key in the --stats-file, which the
    # exporter splits on " : ".
    suite.check(
        all(
            name.isascii()
            and name.isprintable()
            and " " not in name
            and ":" not in name
            for name in names
        ),
        f"every {what} name is usable as a sysfs attribute",
    )


def test_dev_table(suite, fix):
    """The device table describes itself the way the driver reads it."""
    stats = read_table(suite, fix, ionic.ADMIN_STATS_HDRS)
    check_table(suite, stats, "device")
    fix.dev_stats = stats


def test_dev_values(suite, fix):
    """Every counter the device table names has a value behind it."""
    stats = fix.dev_stats
    values = read_values(suite, fix, ionic.ADMIN_STATS_VALS, len(stats))
    suite.check_eq(len(values), len(stats), "a value for every device counter")
    by_name = named_values(stats, values)
    suite.check_eq(len(by_name), len(stats), "every device counter resolves to a value")
    # STATS_VALS is itself an admin command, so this one cannot be zero
    # by the time its own values come back.
    suite.check(by_name.get("commands", 0) > 0, "the device counted admin commands")


# One SEND, one RDMA WRITE and one RDMA READ, of three sizes no two of
# which are equal or a multiple of each other: a counter wired to the
# wrong operation then reads wrong rather than coincidentally right.
BURST_SEND = 768
BURST_WRITE = 88
BURST_READ = 192


def drive_burst(fix):
    """Send, write and read known, distinct byte counts between the QPs.

    Three signalled work requests on A, each waited for, and one receive
    posted on B.  Every counter in both tables moves, so each can be
    checked against a number this function decided.
    """
    fix.driver.mr_write(fix.mr_a, VA_A, bytes(range(256)) * 3)
    fix.driver.post_recv(fix.qp_b, [ionic.Sge(VA_B, MR_LEN, MR_B)])
    send_a_to_b(fix, BURST_SEND)
    fix.driver.wait_cq(fix.cq_b)
    fix.driver.wait_cq(fix.cq_a)

    rdma_a_to_b(fix, ionic.OP_RDMA_WRITE, BURST_WRITE)
    fix.driver.wait_cq(fix.cq_a)

    rdma_a_to_b(fix, ionic.OP_RDMA_READ, BURST_READ)
    fix.driver.wait_cq(fix.cq_a)


def deltas(before, after):
    """A lookup of what each counter moved by."""
    return lambda name: after.get(name, 0) - before.get(name, 0)


def test_dev_values_follow_traffic(suite, fix):
    """Every device counter moves by exactly what the traffic carried."""
    stats = fix.dev_stats
    before = named_values(
        stats, read_values(suite, fix, ionic.ADMIN_STATS_VALS, len(stats))
    )
    drive_burst(fix)
    after = named_values(
        stats, read_values(suite, fix, ionic.ADMIN_STATS_VALS, len(stats))
    )
    delta = deltas(before, after)

    suite.check_eq(delta("total_bytes_sent"), BURST_SEND, "total_bytes_sent moved")
    suite.check_eq(
        delta("total_bytes_received"), BURST_SEND, "total_bytes_received moved"
    )
    suite.check_eq(
        delta("total_bytes_rdma_write"), BURST_WRITE, "total_bytes_rdma_write moved"
    )
    suite.check_eq(
        delta("total_bytes_rdma_read"), BURST_READ, "total_bytes_rdma_read moved"
    )
    # Not an exact count: reading the counters is itself an admin
    # command, so the snapshots move this one on their own.
    suite.check(delta("commands") > 0, "the admin command count moved")


def test_qp_table(suite, fix):
    """The per-QP table describes itself too, and is read per QP."""
    stats = read_table(suite, fix, ionic.ADMIN_QP_STATS_HDRS, id_ver=QP_A)
    check_table(suite, stats, "per-QP")
    fix.qp_stats = stats


def test_qp_values_follow_traffic(suite, fix):
    """Every per-QP counter moves, on the right QP, by the right amount.

    Read for both QPs: the two halves of one transfer are accounted
    separately, and a counter folded onto the wrong QP shows up here as
    a zero that should not be, or the reverse.
    """
    stats = fix.qp_stats

    def snapshot(qp_id):
        return named_values(
            stats,
            read_values(
                suite, fix, ionic.ADMIN_QP_STATS_VALS, len(stats), id_ver=qp_id
            ),
        )

    before_a, before_b = snapshot(QP_A), snapshot(QP_B)
    drive_burst(fix)
    after_a, after_b = snapshot(QP_A), snapshot(QP_B)
    delta_a = deltas(before_a, after_a)
    delta_b = deltas(before_b, after_b)

    # The sender: three signalled work requests, each rung and reaped
    # on its own, and the bytes of each operation under its own name.
    suite.check_eq(delta_a("doorbell_send"), 3, "A rang the send doorbell three times")
    suite.check_eq(delta_a("wqes_processed"), 3, "A processed three WQEs")
    suite.check_eq(delta_a("cqes_posted"), 3, "A posted three CQEs")
    suite.check_eq(delta_a("bytes_sent"), BURST_SEND, "A's bytes_sent")
    suite.check_eq(delta_a("bytes_rdma_write"), BURST_WRITE, "A's bytes_rdma_write")
    suite.check_eq(delta_a("bytes_rdma_read"), BURST_READ, "A's bytes_rdma_read")
    suite.check_eq(delta_a("bytes_received"), 0, "A counted no receive")
    suite.check_eq(delta_a("doorbell_recv"), 0, "A rang no receive doorbell")

    # The receiver: one posted receive, one completion, the SEND's bytes
    # -- and nothing from the WRITE or READ, which name no work request
    # on this side.
    suite.check_eq(delta_b("doorbell_recv"), 1, "B rang the receive doorbell once")
    suite.check_eq(delta_b("cqes_posted"), 1, "B posted one CQE")
    suite.check_eq(delta_b("bytes_received"), BURST_SEND, "B's bytes_received")
    suite.check_eq(delta_b("bytes_sent"), 0, "B counted no send")
    suite.check_eq(delta_b("doorbell_send"), 0, "B rang no send doorbell")
    suite.check_eq(delta_b("wqes_processed"), 0, "B processed no send WQE")
    suite.check_eq(delta_b("bytes_rdma_write"), 0, "B counted no RDMA write")
    suite.check_eq(delta_b("bytes_rdma_read"), 0, "B counted no RDMA read")


def test_qp_values_unknown_qp(suite, fix):
    """Values for a QP that does not exist are refused, not zero-filled."""
    buf = fix.stats_buf
    guard_fill(fix.mem, buf)
    status, _ = fix.driver.admin(
        ionic.ADMIN_QP_STATS_VALS, stats_body(buf, BUF_LEN, UNKNOWN_QP)
    )
    suite.check(status != 0, "QP_STATS_VALS for an unknown QP is refused")
    suite.check(
        guard_tail(fix.mem, buf, 0),
        "a refused QP_STATS_VALS wrote nothing to the buffer",
    )


def test_truncated_buffer(suite, fix):
    """A buffer too small for the whole table gets whole entries only."""
    buf = fix.stats_buf

    fix.mem.write(buf, b"\0" * BUF_LEN)
    guard_fill(fix.mem, buf + STAT_HDR_SIZE, BUF_LEN - STAT_HDR_SIZE)
    status, _ = fix.driver.admin(ionic.ADMIN_STATS_HDRS, stats_body(buf, STAT_HDR_SIZE))
    suite.check_eq(status, 0, "STATS_HDRS into a one-entry buffer completes")
    suite.check(
        guard_tail(fix.mem, buf, STAT_HDR_SIZE),
        "STATS_HDRS wrote only the one entry that fit",
    )

    guard_fill(fix.mem, buf)
    status, _ = fix.driver.admin(ionic.ADMIN_STATS_VALS, stats_body(buf, STAT_VAL_SIZE))
    suite.check_eq(status, 0, "STATS_VALS into a one-value buffer completes")
    suite.check(
        guard_tail(fix.mem, buf, STAT_VAL_SIZE),
        "STATS_VALS wrote only the one value that fit",
    )


def test_rejected_requests(suite, fix):
    """What the device must refuse rather than DMA somewhere unintended."""
    ops = (
        ionic.ADMIN_STATS_HDRS,
        ionic.ADMIN_STATS_VALS,
        ionic.ADMIN_QP_STATS_HDRS,
        ionic.ADMIN_QP_STATS_VALS,
    )
    buf = fix.stats_buf
    for op in ops:
        status, _ = fix.driver.admin(op, stats_body(0, BUF_LEN, QP_A))
        suite.check(status != 0, f"op {op} with no buffer is refused")

        status, _ = fix.driver.admin(op, stats_body(buf, 0, QP_A))
        suite.check(status != 0, f"op {op} with a zero-length buffer is refused")

        # One byte short of struct ionic_v1_admin_stats_cmd.
        short = stats_body(buf, BUF_LEN, QP_A)[:-1]
        status, _ = fix.driver.admin(op, short)
        suite.check(status != 0, f"op {op} with a short command body is refused")


def test_stats_file_agrees(suite, fix):
    """The --stats-file the exporter reads carries the same numbers.

    Prometheus never sees the admin queue; it sees this file. The two
    views come from one set of counters, and this is what says so.
    """
    stats = fix.dev_stats
    # Wait for a flush that cannot predate the traffic above, rather than
    # sleeping for one: the server writes about once a second.
    first = read_stats_file(fix.stats_path)
    deadline = time.monotonic() + 15.0
    current = first
    while current.get("Write count", -1) <= first.get("Write count", -1):
        if time.monotonic() > deadline:
            break
        time.sleep(0.05)
        current = read_stats_file(fix.stats_path)
    suite.check(
        current.get("Write count", -1) > first.get("Write count", -1),
        "the server flushed the stats file",
    )

    values = named_values(
        stats, read_values(suite, fix, ionic.ADMIN_STATS_VALS, len(stats))
    )
    for name in ("total_bytes_sent", "total_bytes_received", "total_bytes_rdma_write"):
        suite.check_eq(
            current.get(name), values.get(name), f"{name} agrees with the stats file"
        )

    # The LIF block the exporter publishes alongside them. A SEND and an
    # RDMA WRITE went out over the emulated wire, so this cannot be zero.
    suite.check(
        current.get("tx_rdma_ucast_bytes", 0) > 0,
        "the LIF block counted the RDMA bytes that went out",
    )
    suite.check(
        current.get("rx_rdma_ucast_bytes", 0) > 0,
        "the LIF block counted the RDMA bytes that came in",
    )


def read_stats_file(path):
    """Parse a --stats-file the way ernic-exporter does: split on ' : '."""
    out = {}
    try:
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                key, sep, val = line.strip().partition(" : ")
                if not sep:
                    continue
                try:
                    out[key.strip()] = int(val.strip())
                except ValueError:
                    out[key.strip()] = val.strip()
    except OSError:
        pass
    return out


TESTS = (
    ("the device stats table", test_dev_table),
    ("device stats values", test_dev_values),
    ("every device value follows traffic", test_dev_values_follow_traffic),
    ("the per-QP stats table", test_qp_table),
    ("every per-QP value follows traffic", test_qp_values_follow_traffic),
    ("values for a QP that does not exist", test_qp_values_unknown_qp),
    ("a buffer too small for the table", test_truncated_buffer),
    ("requests the device must refuse", test_rejected_requests),
    ("the stats file agrees with hw_counters", test_stats_file_agrees),
)


def main():
    """Start a loopback server and read its hardware statistics."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, help="path to the rocm-ernic binary")
    args = parser.parse_args()

    def stats_args(run_dir):
        return ("--stats-file", os.path.join(run_dir, "ionic.stats"))

    def make_fixture(server, run_dir):
        """The tests read the same file the server was told to write."""
        fix = Fixture(server)
        fix.stats_path = os.path.join(run_dir, "ionic.stats")
        fix.stats_buf = fix.mem.alloc(BUF_LEN)
        return fix

    suite = Suite("rocm-ernic ionic hardware statistics over vfio-user")
    run_vfu_suite(suite, args.server, TESTS, make_fixture, stats_args)


if __name__ == "__main__":
    main()
