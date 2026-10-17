"""Controlled server and command processes for the launcher Nix check.

Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
SPDX-License-Identifier: MIT
"""

import json
import os
from pathlib import Path
import signal
import socket
import sys
import time


def main():
    """Record actual argv and process lifetime independently of the launcher."""
    role = sys.argv[1]
    root = Path(os.environ["ERNIC_TEST_ROOT"])
    mode = os.environ[f"ERNIC_TEST_{role.upper()}"]
    (root / f"{role}.json").write_text(json.dumps(sys.argv[2:]))
    (root / f"{role}.pid").write_text(str(os.getpid()))
    if mode == "early":
        return 9
    if role == "command":
        (root / "socket.txt").write_text(os.environ["ERNIC_SOCKET"])
        if mode == "stdin":
            (root / "stdin.txt").write_text(sys.stdin.read())
        if mode != "hang":
            return 7 if mode == "fail" else 0
    signal.signal(
        signal.SIGTERM,
        signal.SIG_IGN if mode == "hang" else lambda *_: sys.exit(0),
    )
    sock = socket.socket(socket.AF_UNIX)
    if role == "server" and mode != "no-socket":
        sock.bind(sys.argv[sys.argv.index("--socket") + 1])
    if mode == "die":
        # Synchronize with the command rather than guessing a startup delay.
        while not (root / "command.pid").exists():
            time.sleep(0.01)
        return 9
    while True:
        time.sleep(0.01)


if __name__ == "__main__":
    sys.exit(main())
