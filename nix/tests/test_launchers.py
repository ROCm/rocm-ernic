"""Table-driven tests of the packaged launcher and guest module argv.

Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
SPDX-License-Identifier: MIT
"""

import json
import os
from pathlib import Path
import signal

# Bandit flags the import and every call. Each call runs, without a shell,
# a program that nix/launcher-tests.nix built and named in ERNIC_TEST_*.
import subprocess  # nosec B404
import sys
import tempfile
import time
import unittest


class LauncherTests(unittest.TestCase):
    """Verify process lifetimes as well as return codes."""

    def test_lifecycle(self):
        """Descriptions and expected exit codes belong to every table row."""
        cases = [
            ("positive", "normal command", "ok", "ok", 0),
            ("positive", "interactive stdin retained", "ok", "stdin", 0),
            ("negative", "command failure", "ok", "fail", 7),
            ("negative", "server startup failure", "early", "ok", 1),
            ("boundary", "readiness deadline", "no-socket", "ok", 1),
            ("corner", "server ignores TERM", "hang", "ok", 0),
            ("corner", "server dies while command waits", "die", "hang", 1),
        ]
        for category, description, server, command, expected in cases:
            with self.subTest(
                category=category, description=description, expected=expected
            ):
                with tempfile.TemporaryDirectory() as scratch:
                    root = Path(scratch)
                    result = subprocess.run(  # nosec B603
                        self.argv(),
                        input="guest console input\n",
                        env=self.env(root, server, command),
                        capture_output=True,
                        text=True,
                        timeout=5,
                        check=False,
                    )
                    self.assertEqual(result.returncode, expected, result.stderr)
                    self.assert_reaped(root)
                    if command == "stdin":
                        self.assertEqual(
                            (root / "stdin.txt").read_text(), "guest console input\n"
                        )
                    self.assertFalse(list(root.glob("ernic-run-*")))
                    if server in ("early", "no-socket"):
                        self.assertFalse((root / "command.pid").exists())

    @staticmethod
    def argv():
        """Invoke the actual Nix writeShellApplication output."""
        return [
            os.environ["ERNIC_TEST_RUNNER"],
            "--",
            sys.executable,
            str(Path(__file__).with_name("fake_ernic.py")),
            "command",
        ]

    @staticmethod
    def env(root, server="ok", command="ok"):
        """Use one independent directory and scenario per test row."""
        return dict(
            os.environ,
            TMPDIR=str(root),
            ERNIC_TEST_ROOT=str(root),
            ERNIC_TEST_SERVER=server,
            ERNIC_TEST_COMMAND=command,
        )

    def assert_reaped(self, root):
        """The supervisor must reap its children, not merely exit itself."""
        for record in root.glob("*.pid"):
            with self.assertRaises(ProcessLookupError, msg=str(record)):
                os.kill(int(record.read_text()), 0)

    def test_usage(self):
        """Invalid input and pre-existing paths must have no child effects."""
        for options, expected in [
            (["--unknown"], 2),
            (["--socket"], 2),
            (["--"], 2),
            (["--backend"], 2),
        ]:
            with self.subTest(description=options, expected=expected):
                with tempfile.TemporaryDirectory() as scratch:
                    root = Path(scratch)
                    result = subprocess.run(  # nosec B603
                        [os.environ["ERNIC_TEST_RUNNER"], *options],
                        env=self.env(root),
                        capture_output=True,
                        timeout=5,
                        check=False,
                    )
                    self.assertEqual(result.returncode, expected)
                    self.assertFalse(list(root.iterdir()))
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            occupied = root / "socket"
            occupied.write_text("caller owned")
            argv = self.argv()
            argv[1:1] = ["--socket", str(occupied)]
            result = subprocess.run(  # nosec B603
                argv, env=self.env(root), capture_output=True, timeout=5, check=False
            )
            self.assertEqual(result.returncode, 1)
            self.assertEqual(occupied.read_text(), "caller owned")
            self.assertFalse((root / "server.pid").exists())

    def test_signal(self):
        """Signals interrupt a waiting command and still reap both children."""
        for sig, expected in [(signal.SIGINT, 130), (signal.SIGTERM, 143)]:
            with self.subTest(description=str(sig), expected=expected):
                with tempfile.TemporaryDirectory() as scratch:
                    root = Path(scratch)
                    with subprocess.Popen(  # nosec B603
                        self.argv(),
                        env=self.env(root, "hang", "hang"),
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        start_new_session=True,
                    ) as proc:
                        deadline = time.monotonic() + 3
                        while not (root / "command.pid").exists():
                            if proc.poll() is not None or time.monotonic() > deadline:
                                self.fail("command never started")
                            time.sleep(0.01)
                        proc.send_signal(sig)
                        proc.communicate(timeout=5)
                        self.assertEqual(proc.returncode, expected)
                    self.assert_reaped(root)
                    self.assertFalse(list(root.glob("ernic-run-*")))

    def test_server_options(self):
        """Forward values verbatim and retain repeat order and the CMD boundary."""
        cases = [
            (
                "positive: every server option",
                [
                    "--backend",
                    "loopback",
                    "--mac",
                    "72:6f:63:6d:00:01",
                    "--stats-file",
                    "stats file.json",
                    "--log-file",
                    "log file.txt",
                    "--tap",
                    "tap0",
                    "--nic-profile",
                    "profile.ini",
                    "--inject",
                    "stats=fail:1/2",
                    "--stats-interval-ms",
                    "0",
                    "--eth-queues",
                    "64",
                ],
                [],
                "ok",
                0,
            ),
            (
                "corner: repeated layered options",
                [
                    "--nic-profile",
                    "first.ini",
                    "--inject",
                    "first rule",
                    "--nic-profile",
                    "second.ini",
                    "--inject",
                    "second rule",
                ],
                [],
                "ok",
                0,
            ),
            (
                "corner: shell metacharacters are data",
                [
                    "--stats-file",
                    "a * ? [x] $HOME `echo x` ; file",
                    "--nic-profile",
                    'quote" and back\\slash',
                ],
                ["--inject", "command-only", "*", ""],
                "ok",
                0,
            ),
            ("boundary: empty option value", ["--stats-file", ""], [], "ok", 0),
            (
                "negative: server rejects option",
                ["--nic-profile", "unsupported.ini"],
                [],
                "early",
                1,
            ),
        ]
        for description, options, command_args, server, expected in cases:
            with self.subTest(description=description, expected=expected):
                with tempfile.TemporaryDirectory() as scratch:
                    root = Path(scratch)
                    socket = str(root / "socket path")
                    argv = self.argv()
                    argv[1:1] = ["--socket", socket, *options]
                    result = subprocess.run(  # nosec B603
                        [*argv, *command_args],
                        env=self.env(root, server),
                        capture_output=True,
                        timeout=5,
                        check=False,
                    )
                    self.assertEqual(result.returncode, expected, result.stderr)
                    self.assertEqual(
                        json.loads((root / "server.json").read_text()),
                        ["--socket", socket, *options],
                    )
                    if server == "early":
                        self.assertFalse((root / "command.pid").exists())
                    else:
                        self.assertEqual(
                            json.loads((root / "command.json").read_text()),
                            command_args,
                        )
                        self.assertEqual((root / "socket.txt").read_text(), socket)
                    self.assert_reaped(root)
                    self.assertFalse(Path(socket).exists())

    def test_missing_option_values(self):
        """Missing values fail before starting a server or creating a directory."""
        for option in [
            "--socket",
            "--backend",
            "--mac",
            "--stats-file",
            "--log-file",
            "--tap",
            "--nic-profile",
            "--inject",
            "--stats-interval-ms",
            "--eth-queues",
        ]:
            with self.subTest(description=option, expected=2):
                with tempfile.TemporaryDirectory() as scratch:
                    root = Path(scratch)
                    result = subprocess.run(  # nosec B603
                        [os.environ["ERNIC_TEST_RUNNER"], option],
                        env=self.env(root),
                        capture_output=True,
                        timeout=5,
                        check=False,
                    )
                    self.assertEqual(result.returncode, 2)
                    self.assertFalse(list(root.iterdir()))

    def test_device_json(self):
        """Generated argv must preserve runtime path characters in JSON."""
        options = json.loads(
            Path(os.environ["ERNIC_TEST_QEMU_OPTIONS"]).read_text(encoding="utf-8")
        )
        for path in ["/run/ernic.sock", '/run/quote" and space\\socket']:
            with self.subTest(description=path, expected="exact JSON path"):
                # The shell expression comes from the evaluated guest module.
                command = "set -- " + options[-1] + '; printf "%s\\n" "$2"'
                result = subprocess.run(  # nosec B603
                    [os.environ["ERNIC_TEST_BASH"], "-euc", command],
                    env=dict(os.environ, ERNIC_SOCKET=path),
                    text=True,
                    capture_output=True,
                    check=True,
                )
                device = json.loads(result.stdout)
                self.assertEqual(device["socket"]["path"], path)
                self.assertEqual(device["driver"], "vfio-user-pci")


if __name__ == "__main__":
    unittest.main()
