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
from ernic import eth
from ernic.check import Suite
from ernic.errors import DeviceError
from ernic.fixture import BaseFixture
from ernic.server import Server, stop_and_check

STATS_RE = re.compile(r"P4 stats: (.*)$", re.MULTILINE)


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


def test_bad_program_refused(suite, server_factory):
    """A program using a feature the engine lacks stops the server starting."""
    prog = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..",
        "data",
        "p4",
        "simple_router.json",
    )
    ctx = server_factory(["--p4-program", prog])
    try:
        with ctx:
            pass
        started = True
    except Exception:  # pylint: disable=broad-except
        started = False
    suite.check(not started, "the server refused to start")
    suite.check(
        "checksums" in ctx.server.log_text(),
        "it said which feature the program used",
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


TESTS = (
    ("guest frames reach the engine", test_engine_sees_guest_frames),
    ("a scatter/gather frame is one frame", test_scatter_gather_frame),
    ("the engine is off without --p4", test_disabled_by_default),
    ("Tx completes with the engine attached", test_tx_completions),
    ("the Rx ring wraps correctly", test_rx_ring_wraps),
    ("a full Tx ring is refused", test_tx_ring_refuses_to_overrun),
    ("a frame makes the round trip through the pipeline", test_round_trip),
    ("a compiled program decides where frames go", test_program_loaded),
    ("a program using an absent feature is refused", test_bad_program_refused),
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
