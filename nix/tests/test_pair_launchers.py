"""Positive, negative, boundary and corner cases for packaged pair launchers.

Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
SPDX-License-Identifier: MIT
"""

import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest


class PairTests(unittest.TestCase):
    """Check exit status, port values and both child lifetimes."""

    def test_ports(self):
        """Every row includes a description and expected decimal output."""
        cases = [
            ("positive: ordinary port", "22", "22"),
            ("boundary: minimum", "1", "1"),
            ("boundary: maximum pair", "65534", "65534"),
            ("corner: leading zero eight", "08", "8"),
            ("corner: leading zero ten", "010", "10"),
            ("corner: many leading zeros", "0" * 100 + "1", "1"),
            ("negative: empty", "", None),
            ("negative: nonnumeric", "1+1", None),
            ("negative: whitespace", " 22", None),
            ("boundary: zero", "0", None),
            ("boundary: second port overflow", "65535", None),
            ("corner: integer overflow", "9" * 100, None),
        ]
        for description, value, expected in cases:
            with self.subTest(description=description, expected=expected):
                result = self.run_launcher([os.environ["ERNIC_TEST_PORT"], value])
                self.assertEqual(result.returncode, 2 if expected is None else 0)
                self.assertEqual(result.stdout.strip(), expected or "")
                self.assertNotIn("value too great", result.stderr)

    @staticmethod
    def run_launcher(argv, env=None, stdin=None):
        """Run one row's launcher with a deadline; the caller checks status."""
        return subprocess.run(
            argv,
            capture_output=True,
            check=False,
            env=env,
            input=stdin,
            text=True,
            timeout=5,
        )

    @staticmethod
    def environment(root, first, second):
        """Keep child records and configuration isolated per row."""
        return dict(
            os.environ,
            ERNIC_PAIR_DIR=str(root),
            ERNIC_TEST_VM1=first,
            ERNIC_TEST_VM2=second,
        )

    def assert_reaped(self, root):
        """Both children must have started and must no longer exist."""
        records = list(root.glob("*.pid"))
        self.assertEqual(len(records), 2)
        for record in records:
            with self.assertRaises(ProcessLookupError):
                os.kill(int(record.read_text()), 0)

    def test_lifecycle(self):
        """Expected statuses distinguish VM failure from orderly shutdown."""
        cases = [
            ("positive: console exit", "INTERACTIVE", "ok", "hang", 0),
            ("positive: console input", "INTERACTIVE", "stdin", "hang", 0),
            ("negative: console fails", "INTERACTIVE", "fail", "hang", 7),
            ("negative: peer fails first", "INTERACTIVE", "hang", "fail", 7),
            ("corner: peer exits first", "INTERACTIVE", "hang", "ok", 1),
            ("positive: both complete", "BOTH", "ok", "ok", 0),
            ("negative: first test fails", "BOTH", "fail", "hang", 7),
            ("negative: second test fails", "BOTH", "hang", "fail", 7),
        ]
        for description, runner, first, second, expected in cases:
            with self.subTest(description=description, expected=expected):
                with tempfile.TemporaryDirectory() as scratch:
                    root = Path(scratch)
                    result = self.run_launcher(
                        [os.environ[f"ERNIC_TEST_{runner}"], "08"],
                        self.environment(root, first, second),
                        "console input\n",
                    )
                    self.assertEqual(result.returncode, expected, result.stderr)
                    self.assert_reaped(root)
                    for node, port in [(1, 8), (2, 9)]:
                        self.assertEqual(
                            (root / f"node{node}.net").read_text(),
                            f"hostfwd=tcp:127.0.0.1:{port}-:22",
                        )
                    if first == "stdin":
                        self.assertEqual(
                            (root / "stdin.txt").read_text(), "console input\n"
                        )

    def test_signal(self):
        """TERM must return 143 and reap even guests that ignore TERM."""
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            # Explicit finally bounds cleanup even if readiness assertions fail.
            # pylint: disable-next=consider-using-with
            process = subprocess.Popen(
                [os.environ["ERNIC_TEST_BOTH"]],
                env=self.environment(root, "hang", "hang"),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                deadline = time.monotonic() + 3
                while len(list(root.glob("*.pid"))) != 2:
                    self.assertLess(time.monotonic(), deadline, "guests not ready")
                    time.sleep(0.01)
                process.send_signal(signal.SIGTERM)
                process.communicate(timeout=5)
                self.assertEqual(process.returncode, 143)
                self.assert_reaped(root)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=2)
                for record in root.glob("*.pid"):
                    try:
                        os.kill(int(record.read_text()), signal.SIGKILL)
                    except ProcessLookupError:
                        pass


if __name__ == "__main__":
    unittest.main(verbosity=2)
