"""Table-driven regression tests for the analysis outcome contract.

Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
SPDX-License-Identifier: MIT
"""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
FAKE_SERVER = """#!/usr/bin/env python3
import os, signal, socket, sys, time
mode = os.environ['SERVER_MODE']
if mode == 'early': sys.exit(9)
s = socket.socket(socket.AF_UNIX)
def stop(*_):
    if mode != 'no-marker': print('Shutdown complete', flush=True)
    sys.exit(9 if mode == 'bad-exit' else 0)
signal.signal(signal.SIGTERM, signal.SIG_IGN if mode == 'hang' else stop)
if mode != 'no-socket': s.bind(sys.argv[sys.argv.index('--socket') + 1])
if mode == 'attach': print('vfu_attach_ctx() failed', flush=True)
while True: time.sleep(.01)
"""
FAKE_CLIENT = """#!/usr/bin/env python3
import os, sys, time
if os.environ['CLIENT_MODE'] == 'hang': time.sleep(60)
sys.exit(7 if os.environ['CLIENT_MODE'] == 'fail' else 0)
"""


def make_driver(root):
    """Use the Nix package, or substitute the same source for local iteration."""
    driver = os.environ.get("ERNIC_TEST_EXERCISE")
    if driver is None:
        script = (HERE / "run-loopback.sh").read_text()
        for key, value in [
            ("STARTUP_ATTEMPTS", "20"),
            ("POLL_SECONDS", ".02"),
            ("CLIENT_TIMEOUT", ".4s"),
            ("SHUTDOWN_TIMEOUT", ".4s"),
        ]:
            script = script.replace(f"@{key}@", value)
        local = root / "runner"
        local.write_text("#!/usr/bin/env bash\nset -u\n" + script)
        local.chmod(0o700)
        driver = str(local)
    return driver


class RunnerTests(unittest.TestCase):
    """Each row checks an outcome, not just the wrapper's exit code."""

    def test_exercise(self):
        """All lifecycle failures must survive the report-file interface."""
        cases = [
            ("positive", "successful client and shutdown", "ok", "ok", "clean"),
            (
                "negative",
                "failed client despite clean shutdown",
                "fail",
                "ok",
                "client failed with status 7",
            ),
            (
                "negative",
                "missing client",
                "missing",
                "ok",
                "client failed with status 127",
            ),
            (
                "negative",
                "server exits before readiness",
                "ok",
                "early",
                "socket never appeared",
            ),
            (
                "boundary",
                "readiness deadline",
                "ok",
                "no-socket",
                "socket never appeared",
            ),
            (
                "boundary",
                "client deadline",
                "hang",
                "ok",
                "client failed with status 124",
            ),
            ("corner", "TERM ignored", "ok", "hang", "server shutdown timed out"),
            (
                "corner",
                "clean log but failed server",
                "ok",
                "bad-exit",
                "server failed with status 9",
            ),
            (
                "negative",
                "missing shutdown marker",
                "ok",
                "no-marker",
                "never logged a clean shutdown",
            ),
            ("negative", "failed attach", "ok", "attach", "failed client attach"),
        ]
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            server, client = root / "server", root / "client"
            server.write_text(
                FAKE_SERVER.replace("#!/usr/bin/env python3", f"#!{sys.executable}")
            )
            client.write_text(
                FAKE_CLIENT.replace("#!/usr/bin/env python3", f"#!{sys.executable}")
            )
            server.chmod(0o700)
            client.chmod(0o700)
            driver = make_driver(root)
            for category, description, client_mode, server_mode, expected in cases:
                with self.subTest(
                    category=category, description=description, expected=expected
                ):
                    env = dict(
                        os.environ,
                        TMPDIR=scratch,
                        CLIENT_MODE=client_mode,
                        SERVER_MODE=server_mode,
                        SERVER_WRAPPER="",
                    )
                    result = subprocess.run(
                        [
                            driver,
                            str(server),
                            (
                                str(root / "missing")
                                if client_mode == "missing"
                                else str(client)
                            ),
                        ],
                        env=env,
                        capture_output=True,
                        text=True,
                        timeout=5,
                        check=False,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    outcome = (root / "exercise.txt").read_text().strip()
                    self.assertIn(expected, outcome, (root / "server.log").read_text())
                    self.assertEqual(
                        outcome.startswith("exercise: clean"), expected == "clean"
                    )
                    self.assertFalse(list(root.glob("ernic-*.sock")))

    def test_scan_report(self):
        """Empty and failed scans must not masquerade as zero findings."""
        valid = {"results": [], "errors": [], "paths": {"scanned": ["src/a.c"]}}
        cases = [
            ("positive", "valid empty scan", json.dumps(valid), "0"),
            (
                "corner",
                "known partial parsing warning",
                json.dumps(
                    dict(
                        valid,
                        errors=[{"level": "warn", "type": ["PartialParsing", []]}],
                    )
                ),
                "0",
            ),
            (
                "negative",
                "unknown warning",
                json.dumps(
                    dict(valid, errors=[{"level": "warn", "type": ["Unknown", []]}])
                ),
                None,
            ),
            (
                "positive",
                "one finding",
                json.dumps(dict(valid, results=[{"check_id": "rule"}])),
                "1",
            ),
            (
                "negative",
                "engine error",
                json.dumps(dict(valid, errors=[{"type": "Fatal error"}])),
                None,
            ),
            ("negative", "malformed JSON", "{", None),
            ("boundary", "empty output", "", None),
            ("negative", "missing results", "{}", None),
            (
                "corner",
                "nothing scanned",
                json.dumps(dict(valid, paths={"scanned": []})),
                None,
            ),
            ("corner", "two documents", json.dumps(valid) + json.dumps(valid), None),
        ]
        for category, description, source, expected in cases:
            with self.subTest(
                category=category, description=description, expected=expected
            ):
                result = subprocess.run(
                    ["jq", "-esr", "-f", str(HERE / "semgrep-report.jq")],
                    input=source,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                if expected is None:
                    self.assertNotEqual(result.returncode, 0)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout.strip(), expected)


if __name__ == "__main__":
    unittest.main()
