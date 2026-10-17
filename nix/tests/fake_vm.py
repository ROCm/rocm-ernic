"""Controlled VM process for the packaged pair supervisor tests.

Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
SPDX-License-Identifier: MIT
"""

import os
from pathlib import Path
import signal
import sys
import time


def main():
    """Publish readiness after installing handlers; synchronize both guests."""
    node = sys.argv[1]
    root = Path(os.environ["ERNIC_PAIR_DIR"])
    mode = os.environ[f"ERNIC_TEST_VM{node}"]
    if mode == "hang":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    (root / f"node{node}.pid").write_text(str(os.getpid()))
    (root / f"node{node}.net").write_text(os.environ["QEMU_NET_OPTS"])
    for _ in range(200):
        if all((root / f"node{i}.pid").exists() for i in (1, 2)):
            break
        time.sleep(0.01)
    else:
        return 8
    if mode == "stdin":
        (root / "stdin.txt").write_text(sys.stdin.read())
    if mode == "hang":
        while True:
            time.sleep(1)
    return 7 if mode == "fail" else 0


if __name__ == "__main__":
    sys.exit(main())
