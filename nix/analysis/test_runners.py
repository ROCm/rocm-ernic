# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Table-driven tests for the outcomes the analysis runners report.

test_exercise runs run-loopback-exercise (run-loopback.sh) against a fake
server and client and checks the outcome it records in exercise.txt.
test_scan_report feeds semgrep-report.jq made-up semgrep output and checks
that it fails on anything but a complete, well-formed scan.

The analysis-runners check in flake.nix runs this with ERNIC_TEST_EXERCISE
set to the packaged driver. Without it, the tests build the driver from
run-loopback.sh themselves, so they also run outside Nix, given bash, jq,
and the GNU coreutils and util-linux commands the driver uses.
"""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent

# Stands in for the server. SERVER_MODE selects how it misbehaves:
#   ok         binds the socket and logs "Shutdown complete" on SIGTERM
#   early      exits with status 9 before binding the socket
#   no-socket  never binds the socket
#   attach     waits for the client to connect, then logs the failed client
#              attach message and exits with status 1, as the real server's
#              err() call does
#   hang       ignores SIGTERM
#   bad-exit   logs "Shutdown complete" but exits with status 9
#   no-marker  exits on SIGTERM without logging "Shutdown complete"
FAKE_SERVER = """\
import os
import signal
import socket
import sys
import time

mode = os.environ["SERVER_MODE"]
if mode == "early":
    sys.exit(9)


def stop(*_):
    if mode != "no-marker":
        print("Shutdown complete", flush=True)
    sys.exit(9 if mode == "bad-exit" else 0)


signal.signal(signal.SIGTERM, signal.SIG_IGN if mode == "hang" else stop)
sock = socket.socket(socket.AF_UNIX)
if mode != "no-socket":
    sock.bind(sys.argv[sys.argv.index("--socket") + 1])
if mode == "attach":
    sock.listen()
    sock.accept()
    print("vfu_attach_ctx() failed", flush=True)
    sys.exit(1)
while True:
    time.sleep(0.01)
"""

# Stands in for the client. It connects to the socket if the server is
# listening, which only the attach server does. CLIENT_MODE is ok (exits
# with status 0), fail (exits with status 7), or hang (outlives the
# driver's client timeout).
FAKE_CLIENT = """\
import os
import socket
import sys
import time

try:
    socket.socket(socket.AF_UNIX).connect(sys.argv[sys.argv.index("--socket") + 1])
except OSError:
    pass
if os.environ["CLIENT_MODE"] == "hang":
    time.sleep(60)
sys.exit(7 if os.environ["CLIENT_MODE"] == "fail" else 0)
"""


def make_driver(root):
    """Return the packaged driver, or build one in root from run-loopback.sh.

    The built one gets the short deadlines that flake.nix gives the packaged
    one, and the shebang and `set -u` that writeShellApplication adds.
    """
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
        # (category, description, CLIENT_MODE, SERVER_MODE, text expected in
        # exercise.txt). CLIENT_MODE "missing" passes a client that doesn't
        # exist.
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
            (
                "corner",
                "failed attach and failed client",
                "fail",
                "attach",
                "failed client attach",
            ),
        ]
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            server, client = root / "server", root / "client"
            server.write_text(f"#!{sys.executable}\n{FAKE_SERVER}")
            client.write_text(f"#!{sys.executable}\n{FAKE_CLIENT}")
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
        # (category, description, semgrep's output, the finding count
        # expected, or None if semgrep-report.jq must fail)
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
