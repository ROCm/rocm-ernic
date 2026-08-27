#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Smoke-test: start rocm-ernic in --ionic mode, verify the socket appears,
# and check the server log for the expected ionic startup banner.
# Exit 0 = pass, exit 1 = fail, exit 77 = skip.

set -euo pipefail

SERVER="${1:-}"
if [ -z "$SERVER" ]; then
    echo "Usage: $0 <server_path>"
    exit 1
fi
if [ ! -x "$SERVER" ]; then
    echo "SKIP: server not found: $SERVER"
    exit 77
fi

SOCKET="/tmp/vfio-ionic-smoke-$$.sock"
LOG="/tmp/vfio-ionic-smoke-$$.log"
SERVER_PID=""

cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -f "$SOCKET" "$LOG"
}
trap cleanup EXIT INT TERM

echo "=== ionic server smoke test ==="
"$SERVER" --ionic --backend loopback --socket "$SOCKET" > "$LOG" 2>&1 &
SERVER_PID=$!

# Wait up to 5 seconds for the socket to appear
for i in $(seq 1 10); do
    sleep 0.5
    if [ -S "$SOCKET" ]; then
        echo "✓ socket appeared after ${i}×0.5s"
        break
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "✗ server exited early"
        cat "$LOG"
        exit 1
    fi
done

if [ ! -S "$SOCKET" ]; then
    echo "✗ socket never appeared"
    cat "$LOG"
    exit 1
fi

# Verify ionic mode banner in the log
if grep -q "ionic emulation initialized" "$LOG" && \
   grep -q "VID:DID 0x1022:0x8001" "$LOG"; then
    echo "✓ ionic banner found in server log"
else
    echo "✗ ionic banner missing from log"
    cat "$LOG"
    exit 1
fi

echo "=== PASSED ==="
