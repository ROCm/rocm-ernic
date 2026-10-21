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
from ernic import eth, ionic, vfu
from ernic.check import Suite
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


class Fixture:
    """A connected client and a brought-up driver, with an Ethernet Tx queue."""

    # pylint: disable=too-few-public-methods
    def __init__(self, server, sg=False):
        self.client = vfu.VfioUserClient(server.socket_path).connect(timeout=30)
        self.mem = vfu.GuestMemory(size=32 << 20)
        self.client.dma_map(self.mem)
        self.driver = ionic.IonicDriver(self.client, self.mem)
        self.driver.bringup()
        self.driver.eth_q_init(eth.QTYPE_ETH_TXQ, depth_log2=3, sg=sg)

    def close(self):
        """Drop the client, as a guest shutting down does."""
        self.mem.close()
        self.client.close()


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


def test_tx_completions(suite, server_factory):
    """The Tx ring completes with the engine attached, as it does without."""
    with server_factory(["--p4"]) as server:
        fix = Fixture(server)
        try:
            for i in range(5):
                posted = fix.driver.post_tx(bytes([0x33]) * (64 + i))
                got = fix.driver.wait_tx()
                suite.check_eq(got[0], posted, "completion names the descriptor")
        finally:
            fix.close()
        stop_server(server)


TESTS = (
    ("guest frames reach the engine", test_engine_sees_guest_frames),
    ("a scatter/gather frame is one frame", test_scatter_gather_frame),
    ("the engine is off without --p4", test_disabled_by_default),
    ("Tx completes with the engine attached", test_tx_completions),
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
