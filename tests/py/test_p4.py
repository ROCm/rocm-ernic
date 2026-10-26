#!/usr/bin/env python3
"""Drive the P4 match-action engine on the emulated wire, over the socket.

The engine's own unit test covers it as a function: frames in, verdict out.
What this adds is the path a VM takes to reach it -- the vfio-user protocol,
the server's main loop, real DMA out of the Tx ring, and the Tx filter chain
the engine installs on the live device.

No program can be loaded yet, so the engine forwards every frame unchanged
to the other port. With no TAP attached that forward fails, which is the
only observable the engine has today; the counters come out of the server's
shutdown statistics line.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# pylint: disable=wrong-import-position
from ernic import eth, ionic, p4
from ernic.check import Suite
from ernic.errors import DeviceError
from ernic.fixture import BaseFixture
from ernic.server import Server, ServerFailed, stop_and_check

STATS_RE = re.compile(r"P4 stats: (.*)$", re.MULTILINE)

# What the server exits with when it refuses its arguments: EXIT_FAILURE.
REFUSED_STATUS = 1

# The first line of a sanitizer report, which goes to the server's log.
SANITIZER_RE = re.compile(
    r"ERROR: \w+Sanitizer|WARNING: ThreadSanitizer|runtime error:"
)

# P4_PROGRAM_MAX in src/p4/p4_engine.h: the largest program the engine takes.
P4_PROGRAM_MAX = 4 * 1024 * 1024


def p4_stats(server):
    """Parse the server's shutdown statistics line into a dict.

    Returns None when the line is absent, which is what a server built
    without the engine, or started without --p4, produces.
    """
    match = STATS_RE.search(server.log_text())
    if match is None:
        return None
    out = {}
    for field in match.group(1).split():
        key, _, value = field.partition("=")
        out[key] = int(value)
    return out


class Fixture(BaseFixture):
    """A brought-up driver with an Ethernet Tx queue."""

    # pylint: disable=too-few-public-methods
    def __init__(self, server, sg=False):
        super().__init__(server)
        self.driver.eth_q_init(eth.QTYPE_ETH_TXQ, depth_log2=3, sg=sg)


def run_frames(server, frames, sg=False):
    """Transmit @frames through a fresh fixture and return the driver's view."""
    fix = Fixture(server, sg=sg)
    try:
        for frame, split in frames:
            fix.driver.post_tx(frame, split=split)
            fix.driver.wait_tx()
    finally:
        fix.close()


def test_engine_sees_guest_frames(suite, server_factory):
    """Every frame the guest transmits reaches the pipeline."""
    with server_factory(["--p4"]) as server:
        run_frames(server, [(bytes([0x11]) * 128, None)] * 3)
        stats = stop_server(server)

    suite.check(stats is not None, "the server reported P4 statistics")
    suite.check_eq(stats["host_in"], 3, "three guest frames reached the engine")
    suite.check_eq(stats["uplink_out"], 3, "all three were forwarded to the uplink")
    # No TAP, so the forward has nowhere to go. That it fails here rather
    # than silently succeeding is the point.
    suite.check_eq(stats["egress_errors"], 3, "the uplink forward failed with no TAP")
    suite.check_eq(stats["to_net"], 0, "nothing left through an absent backend")
    suite.check_eq(stats["to_guest"], 0, "nothing was looped back to the guest")


def test_scatter_gather_frame(suite, server_factory):
    """A frame split over SG elements arrives at the pipeline as one frame."""
    with server_factory(["--p4"]) as server:
        frame = bytes(range(256)) * 2
        run_frames(server, [(frame, [100, 50])], sg=True)
        stats = stop_server(server)

    suite.check_eq(stats["host_in"], 1, "the gathered frame counted once")
    suite.check_eq(stats["too_big"], 0, "the gathered frame was not oversized")


def test_disabled_by_default(suite, server_factory):
    """Without --p4 the engine is not on the wire at all."""
    with server_factory([]) as server:
        run_frames(server, [(bytes([0x22]) * 64, None)])
        stats = stop_server(server)

    suite.check(stats is None, "no P4 statistics without --p4")


def test_round_trip(suite, server_factory):
    """A frame goes out of the Tx ring and comes back on the Rx ring.

    This is the whole path: the guest transmits, the P4 program sets
    egress_spec back to the host port, and the frame is injected into the
    guest's receive ring. No TAP, no peer, no second VM -- the pipeline
    itself is the loop. It is what a VM test does, without the VM.
    """
    prog = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "data", "p4", "loopback.json"
    )
    with server_factory(["--p4-program", prog]) as server:
        fix = Fixture(server)
        try:
            fix.driver.eth_q_init(eth.QTYPE_ETH_RXQ, depth_log2=3)
            fix.driver.post_rx_buffers(count=4, size=2048)

            sent = bytes([0xDE, 0xAD, 0xBE, 0xEF]) + bytes(range(60))
            fix.driver.post_tx(sent)
            fix.driver.wait_tx()

            got = fix.driver.wait_rx(timeout=5.0)
            suite.check(got is not None, "a frame came back")
            suite.check_eq(len(got), len(sent), "it is the length that was sent")
            suite.check(got == sent, "it is the frame that was sent, byte for byte")
        finally:
            fix.close()
        stats = stop_server(server)

    suite.check_eq(stats["host_in"], 1, "one frame entered the pipeline")
    suite.check_eq(stats["host_out"], 1, "the program sent it to the host port")
    suite.check_eq(stats["to_guest"], 1, "it was injected into the guest ring")


def test_program_loaded(suite, server_factory):
    """A compiled program loads at startup and then decides where frames go."""
    prog = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "data", "p4", "loopback.json"
    )
    with server_factory(["--p4-program", prog]) as server:
        run_frames(server, [(bytes([0x44]) * 128, None)] * 2)
        log = server.log_text()
        stats = stop_server(server)

    suite.check("running" in log, "the server reported the program it loaded")
    suite.check_eq(stats["host_in"], 2, "both frames reached the engine")
    # loopback.json leaves egress_spec at 0, which is the host port, so the
    # frames go back to the guest rather than out of an absent uplink.
    suite.check_eq(stats["host_out"], 2, "the program sent them to the host port")
    suite.check_eq(stats["to_guest"], 2, "they were injected into the guest ring")
    suite.check_eq(stats["egress_errors"], 0, "no frame needed an absent backend")


def start_refused(server_factory, extra_args):
    """Start a server with @extra_args; return its exit status and its log.

    A refusal is the server exiting before its socket appears, which
    start() raises as ServerFailed. Anything else -- a server that hung
    on its way out, or a fault in the test -- is not a refusal, and
    propagates. A server that does come up is stopped and checked, and
    its status, 0 if it shut down cleanly, is returned like any other.
    """
    ctx = server_factory(extra_args)
    try:
        with ctx:
            pass
    except ServerFailed:
        pass
    return ctx.server.proc.returncode, ctx.server.log_text()


def check_refused(suite, server_factory, extra_args, what):
    """Check that a server given @extra_args refuses to start; return its log.

    The status alone doesn't show it: ASan, LSan and UBSan exit with the
    same status as a refusal, so a server that refused and then crashed
    on its way out is told apart only by the report in its log.
    """
    status, log = start_refused(server_factory, extra_args)
    suite.check_eq(status, REFUSED_STATUS, f"{what} stopped the server starting")
    suite.check(
        SANITIZER_RE.search(log) is None,
        f"the server refused {what} without a sanitizer report",
    )
    return log


def test_bad_program_refused(suite, server_factory):
    """A program using a feature the engine lacks stops the server starting."""
    prog = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..",
        "data",
        "p4",
        "simple_router.json",
    )
    log = check_refused(
        suite, server_factory, ["--p4-program", prog], "an unsupported program"
    )
    suite.check("checksums" in log, "it said which feature the program used")


def test_program_file_refused(suite, server_factory):
    """A --p4-program file that cannot be a program stops the server starting.

    Each is refused before the engine sees a byte of it: one that is not
    there, one that is empty, and one a byte over the engine's limit.
    """
    with tempfile.TemporaryDirectory(prefix="ernic-p4-file-") as tmp:
        empty = os.path.join(tmp, "empty.json")
        big = os.path.join(tmp, "big.json")
        with open(empty, "wb"):
            pass
        with open(big, "wb") as f:
            f.truncate(P4_PROGRAM_MAX + 1)
        rows = (
            (
                "a missing file",
                os.path.join(tmp, "missing.json"),
                "cannot open P4 program",
            ),
            ("an empty file", empty, "is 0 bytes; the limit is"),
            (
                "a file over the limit",
                big,
                f"is {P4_PROGRAM_MAX + 1} bytes; the limit is",
            ),
        )
        for what, path, want in rows:
            log = check_refused(suite, server_factory, ["--p4-program", path], what)
            suite.check(want in log, f"it said why it refused {what}")


def test_bad_guest_scope(suite, server_factory):
    """A --p4-guest-scope that is not a scope stops the server starting."""
    log = check_refused(
        suite, server_factory, ["--p4-guest-scope", "all"], "an unknown scope"
    )
    suite.check(
        "--p4-guest-scope must be one of" in log,
        "it said why it refused the scope",
    )


def test_tx_completions(suite, server_factory):
    """The Tx ring completes with the engine attached, as it does without.

    Three times round a depth-8 ring, not five posts into it: the number
    post_tx returns and the slot the completion names agree for the first
    lap whatever either of them means, so a test that stops short of the
    wrap passes with the two swapped.
    """
    with server_factory(["--p4"]) as server:
        fix = Fixture(server)
        try:
            depth = fix.driver.eth_tx.depth
            for i in range(3 * depth):
                posted = fix.driver.post_tx(bytes([0x33]) * (64 + i))
                got = fix.driver.wait_tx()
                suite.check_eq(got[0], posted, f"completion {i} names the slot")
        finally:
            fix.close()
        stop_server(server)


def test_jumbo_frame_round_trip(suite, server_factory):
    """A frame at the largest size the device advertises survives the pipeline.

    The P4 port claims every frame, so a frame the engine refuses is one
    the guest sent and nothing forwards -- silently, and only with --p4
    on. The engine's own size test is written in terms of its own limit
    and so passes at any value of it; this one is written in terms of
    what the device tells the guest it may send.
    """
    prog = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "data", "p4", "loopback.json"
    )
    with server_factory(["--p4-program", prog]) as server:
        fix = Fixture(server)
        try:
            fix.driver.eth_q_init(eth.QTYPE_ETH_RXQ, depth_log2=3)
            fix.driver.post_rx_buffers(count=2, size=eth.ETH_MTU_MAX)

            # The largest frame a conforming guest can put on this ring:
            # one tagged frame at the largest MTU the driver allows. The
            # port refuses anything longer, as a real MAC does.
            max_frame = fix.driver.max_frame_size()
            suite.check(
                1518 < max_frame <= eth.ETH_MTU_MAX,
                f"the device advertises jumbo frames ({max_frame} bytes)",
            )
            sent = bytes(i & 0xFF for i in range(max_frame))
            fix.driver.post_tx(sent)
            fix.driver.wait_tx()

            got = fix.driver.wait_rx(timeout=5.0)
            suite.check(got == sent, "the jumbo frame came back intact")
        finally:
            fix.close()
        stop_server(server)


def test_rx_ring_wraps(suite, server_factory):
    """A frame received after the Rx ring wraps is read from its own buffer.

    The completion names a ring slot. Looked up instead in the list of
    every buffer ever posted, the two agree for the first lap and then
    diverge, and the frame is read out of the oldest buffer rather than
    the one it was delivered into -- which, since the Rx buffers all start
    zeroed, reads as a frame of the right length and the wrong contents.
    """
    prog = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "data", "p4", "loopback.json"
    )
    with server_factory(["--p4-program", prog]) as server:
        fix = Fixture(server)
        try:
            fix.driver.eth_q_init(eth.QTYPE_ETH_RXQ, depth_log2=3)
            depth = fix.driver.eth_rx.depth
            for i in range(2 * depth + 1):
                fix.driver.post_rx_buffers(count=1, size=2048)
                sent = bytes([i & 0xFF]) * 64
                fix.driver.post_tx(sent)
                fix.driver.wait_tx()
                got = fix.driver.wait_rx(timeout=5.0)
                suite.check(got == sent, f"frame {i} came back from its buffer")
        finally:
            fix.close()
        stop_server(server)


def test_tx_ring_refuses_to_overrun(suite, server_factory):
    """Posting over an uncompleted descriptor is refused, not silently done.

    The device reads a descriptor when it reaches it, not when the
    doorbell rings, so overwriting a slot whose frame has not been sent
    loses that frame with no error anywhere near the cause.
    """
    with server_factory(["--p4"]) as server:
        fix = Fixture(server)
        try:
            depth = fix.driver.eth_tx.depth
            for _ in range(depth):
                fix.driver.post_tx(bytes(64))
            try:
                fix.driver.post_tx(bytes(64))
                suite.check(False, "a full Tx ring refused another descriptor")
            except DeviceError as exc:
                suite.check("full" in str(exc), "it said the ring was full")
            # Draining it makes room again, so this is a bound, not a wall.
            for _ in range(depth):
                fix.driver.wait_tx()
            fix.driver.post_tx(bytes(64))
            fix.driver.wait_tx()
            suite.check(True, "the ring takes descriptors again once drained")
        finally:
            fix.close()
        stop_server(server)


# ---- Format conversion on the RDMA path --------------------------------
#
# Everything above drives the Ethernet path, which is where the engine
# decides where a frame goes.  These drive the other plane: an RDMA WRITE
# the program turns into an elementwise format conversion.  The whole
# control path is the guest's -- the program is pushed down and the table
# entry installed through the device commands, not through --p4-program --
# so this is the Python mirror of what the VM lane does with a real
# driver, and it runs with no VM at all.

CVT_CQ_A, CVT_CQ_B = 11, 12
CVT_QP_A, CVT_QP_B = 60, 61
CVT_MR_SRC, CVT_MR_DST = 0x05000001, 0x05000002
CVT_VA_SRC = 0x7E1000000000
CVT_VA_DST = 0x7E2000000000
CVT_MR_LEN = 4 * 4096

# Four fp16 encodings with something to say: 1.0, 2.0, a NaN, and an
# infinity, which E4M3 has no room for and must saturate rather than
# poison the buffer with.
FP16_SAMPLES = (0x3C00, 0x4000, 0x7E00, 0x7C00)
# What each becomes in E4M3, per the conventions in p4_fmt.h.
E4M3_WANT = (0x38, 0x40, 0x7F, 0x7E)


def fp16_buffer(n_elems):
    """@n_elems little-endian fp16 values cycling through FP16_SAMPLES."""
    out = bytearray()
    for i in range(n_elems):
        out += int(FP16_SAMPLES[i % len(FP16_SAMPLES)]).to_bytes(2, "little")
    return bytes(out)


class CvtFixture(BaseFixture):
    """Two connected RC queue pairs and a source and destination region."""

    # pylint: disable=too-few-public-methods
    def __init__(self, server):
        super().__init__(server)
        self.cq_a = self.driver.create_cq(CVT_CQ_A)
        self.cq_b = self.driver.create_cq(CVT_CQ_B)
        self.qp_a = self.driver.create_qp(CVT_QP_A, ionic.QPT_RC, self.cq_a, self.cq_a)
        self.qp_b = self.driver.create_qp(CVT_QP_B, ionic.QPT_RC, self.cq_b, self.cq_b)
        hdr = ionic.roce_hdr_v4(ionic.ipv4("192.168.0.1"), ionic.ipv4("192.168.0.2"))
        self.driver.connect_qp(self.qp_a, CVT_QP_B, hdr)
        self.driver.connect_qp(self.qp_b, CVT_QP_A, hdr)
        # Reversed pages on the destination, so a write that ignores the
        # page table lands somewhere else and the comparison says so.
        self.mr_src = self.driver.create_mr(CVT_MR_SRC, CVT_VA_SRC, CVT_MR_LEN)
        self.mr_dst = self.driver.create_mr(
            CVT_MR_DST, CVT_VA_DST, CVT_MR_LEN, reverse=True
        )


def convert_program():
    """The compiled conversion program, as bytes."""
    path = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..",
        "data",
        "p4",
        "fp8_convert.json",
    )
    with open(path, "rb") as fh:
        return fh.read()


def test_cvt_guest_loads_program(suite, server_factory):
    """A guest pushes the conversion program down and installs an entry.

    The control path only: that the surface answers, that the program the
    compiler emitted is accepted through the device commands, and that an
    entry naming the action it declares is installed.
    """
    with server_factory(["--p4", "--p4-guest-scope", "load"]) as server:
        fix = CvtFixture(server)
        try:
            ident = p4.ident(fix.driver)
            suite.check(ident is not None, "the P4 surface answered IDENT")
            if ident:
                suite.check_eq(
                    ident["scope"],
                    p4.P4_SCOPE_LOAD,
                    "it reports the scope the operator granted",
                )
            p4.load_program(fix.driver, convert_program())
            status = p4.entry(
                fix.driver,
                (p4.P4_UPDATE_INSERT, "Ing.policy", "Ing.convert"),
                matches=[bytes([ionic.OP_RDMA_WRITE])],
                params=[bytes([p4.P4_FMT_FP16]), bytes([p4.P4_FMT_FP8_E4M3])],
            )
            suite.check_eq(status, 0, "the conversion entry was installed")
        finally:
            fix.close()
        stop_server(server)


def test_cvt_narrowing(suite, server_factory):
    """fp16 in one region becomes fp8 in a region half as long.

    The work request is an ordinary one-entry RDMA WRITE; only the table
    entry makes it a conversion.
    """
    n_elems = 512
    src = fp16_buffer(n_elems)

    with server_factory(["--p4", "--p4-guest-scope", "load"]) as server:
        fix = CvtFixture(server)
        try:
            p4.load_program(fix.driver, convert_program())
            suite.check_eq(
                p4.entry(
                    fix.driver,
                    (p4.P4_UPDATE_INSERT, "Ing.policy", "Ing.convert"),
                    matches=[bytes([ionic.OP_RDMA_WRITE])],
                    params=[bytes([p4.P4_FMT_FP16]), bytes([p4.P4_FMT_FP8_E4M3])],
                ),
                0,
                "the fp16 to E4M3 entry was installed",
            )
            fix.driver.mr_write(fix.mr_src, CVT_VA_SRC, src)
            # Poison the destination, so writing past the result shows.
            fix.driver.mr_write(fix.mr_dst, CVT_VA_DST, b"\xa5" * len(src))

            fix.driver.post_send(
                fix.qp_a,
                ionic.OP_RDMA_WRITE,
                flags=ionic.WQE_SIGNALLED,
                sge=[ionic.Sge(CVT_VA_SRC, len(src), CVT_MR_SRC)],
                remote_va=CVT_VA_DST,
                rkey=CVT_MR_DST,
            )
            comp = fix.driver.wait_cq(fix.cq_a)
            suite.check_eq(comp.status, 0, "the conversion completed")

            got = fix.driver.mr_read(fix.mr_dst, CVT_VA_DST, len(src))
            want = bytes(E4M3_WANT[i % len(E4M3_WANT)] for i in range(n_elems))
            suite.check_eq(got[:n_elems], want, "every fp16 element became its fp8")
            # The result is half the source; the rest must be untouched.
            suite.check_eq(
                got[n_elems:],
                b"\xa5" * (len(src) - n_elems),
                "nothing was written past the end of the result",
            )
        finally:
            fix.close()
        stop_server(server)


def test_cvt_widening(suite, server_factory):
    """The same program widens, because the entry names both formats.

    fp4 is the narrowest format and fp16 one of the widest, so this is
    the direction the earlier design could not express at all: the
    destination is four times the source.
    """
    # Eight fp4 elements: 1.0, 2.0, 4.0, 6.0 twice over.  Two to a byte,
    # low nibble first, so 0x42 is (1.0, 2.0).
    src = bytes([0x42, 0x76, 0x42, 0x76])
    n_elems = len(src) * 2
    want_f16 = (0x3C00, 0x4000, 0x4400, 0x4600)  # 1.0, 2.0, 4.0, 6.0

    with server_factory(["--p4", "--p4-guest-scope", "load"]) as server:
        fix = CvtFixture(server)
        try:
            p4.load_program(fix.driver, convert_program())
            suite.check_eq(
                p4.entry(
                    fix.driver,
                    (p4.P4_UPDATE_INSERT, "Ing.policy", "Ing.convert"),
                    matches=[bytes([ionic.OP_RDMA_WRITE])],
                    params=[bytes([p4.P4_FMT_FP4_E2M1]), bytes([p4.P4_FMT_FP16])],
                ),
                0,
                "the fp4 to fp16 entry was installed",
            )
            fix.driver.mr_write(fix.mr_src, CVT_VA_SRC, src)
            fix.driver.mr_write(fix.mr_dst, CVT_VA_DST, b"\x00" * 64)

            fix.driver.post_send(
                fix.qp_a,
                ionic.OP_RDMA_WRITE,
                flags=ionic.WQE_SIGNALLED,
                sge=[ionic.Sge(CVT_VA_SRC, len(src), CVT_MR_SRC)],
                remote_va=CVT_VA_DST,
                rkey=CVT_MR_DST,
            )
            comp = fix.driver.wait_cq(fix.cq_a)
            suite.check_eq(comp.status, 0, "the widening completed")

            got = fix.driver.mr_read(fix.mr_dst, CVT_VA_DST, 2 * n_elems)
            want = bytearray()
            for i in range(n_elems):
                want += int(want_f16[i % 4]).to_bytes(2, "little")
            suite.check_eq(got, bytes(want), "every fp4 element became its fp16")
        finally:
            fix.close()
        stop_server(server)


def test_cvt_plain_write_untouched(suite, server_factory):
    """A two-entry gather list is not the conversion shape.

    The table is keyed on the opcode, which cannot tell a conversion from
    an ordinary write, so the action fires for both. The extern declines
    anything that is not one source region and the operation stays the
    ordinary gathered write it looks like, rather than failing the QP.
    """
    half = fp16_buffer(64)

    with server_factory(["--p4", "--p4-guest-scope", "load"]) as server:
        fix = CvtFixture(server)
        try:
            p4.load_program(fix.driver, convert_program())
            # Without the entry the table's default passes everything
            # through, and the checks below would pass without the
            # extern ever being asked to decline.
            suite.check_eq(
                p4.entry(
                    fix.driver,
                    (p4.P4_UPDATE_INSERT, "Ing.policy", "Ing.convert"),
                    matches=[bytes([ionic.OP_RDMA_WRITE])],
                    params=[bytes([p4.P4_FMT_FP16]), bytes([p4.P4_FMT_FP8_E4M3])],
                ),
                0,
                "the conversion entry was installed",
            )
            fix.driver.mr_write(fix.mr_src, CVT_VA_SRC, half + half)
            fix.driver.post_send(
                fix.qp_a,
                ionic.OP_RDMA_WRITE,
                flags=ionic.WQE_SIGNALLED,
                sge=[
                    ionic.Sge(CVT_VA_SRC, len(half), CVT_MR_SRC),
                    ionic.Sge(CVT_VA_SRC + len(half), len(half), CVT_MR_SRC),
                ],
                remote_va=CVT_VA_DST,
                rkey=CVT_MR_DST,
            )
            comp = fix.driver.wait_cq(fix.cq_a)
            suite.check_eq(
                comp.status, 0, "a two-entry write under the entry was not refused"
            )
            suite.check_eq(
                fix.driver.mr_read(fix.mr_dst, CVT_VA_DST, 2 * len(half)),
                half + half,
                "it stayed an ordinary gathered transfer",
            )
        finally:
            fix.close()
        stop_server(server)


TESTS = (
    ("guest frames reach the engine", test_engine_sees_guest_frames),
    ("a scatter/gather frame is one frame", test_scatter_gather_frame),
    ("the engine is off without --p4", test_disabled_by_default),
    ("Tx completes with the engine attached", test_tx_completions),
    ("a jumbo frame makes the round trip", test_jumbo_frame_round_trip),
    ("the Rx ring wraps correctly", test_rx_ring_wraps),
    ("a full Tx ring is refused", test_tx_ring_refuses_to_overrun),
    ("a frame makes the round trip through the pipeline", test_round_trip),
    ("a compiled program decides where frames go", test_program_loaded),
    ("a program using an absent feature is refused", test_bad_program_refused),
    ("a file that cannot be a program is refused", test_program_file_refused),
    ("a scope that is not one is refused", test_bad_guest_scope),
    ("a guest loads the conversion program", test_cvt_guest_loads_program),
    ("fp16 narrows to fp8 on the RDMA path", test_cvt_narrowing),
    ("fp4 widens to fp16 on the RDMA path", test_cvt_widening),
    ("a two-entry write stays an ordinary transfer", test_cvt_plain_write_untouched),
)


def stop_server(server):
    """Stop @server and return its parsed P4 statistics, or None."""
    server.stop()
    return p4_stats(server)


def main():
    """Start servers with and without --p4 and drive the Ethernet Tx path."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, help="path to the rocm-ernic binary")
    args = parser.parse_args()

    suite = Suite("rocm-ernic P4 engine tests over vfio-user")
    with tempfile.TemporaryDirectory(prefix="ernic-p4-") as run_dir:
        counter = [0]

        class ServerCtx:
            """One server per test, so the statistics line starts clean."""

            def __init__(self, extra_args):
                counter[0] += 1
                tag = f"p4-{counter[0]}"
                self.server = Server(
                    args.server,
                    os.path.join(run_dir, f"{tag}.sock"),
                    log_path=os.path.join(run_dir, f"{tag}.log"),
                    extra_args=extra_args,
                )

            def __enter__(self):
                self.server.start(timeout=60)
                return self.server

            def __exit__(self, *_exc):
                if self.server.is_running():
                    stop_and_check(suite, self.server)
                return False

        for name, func in TESTS:
            suite.run(name, func, ServerCtx)
    suite.finish()


if __name__ == "__main__":
    main()
