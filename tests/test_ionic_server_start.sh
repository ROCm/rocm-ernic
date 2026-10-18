#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Smoke-test: start rocm-ernic, verify the socket appears,
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
# A command to run the server under, such as valgrind and its options. It's
# split on whitespace, so no word of it can contain a space.
read -r -a SERVER_WRAPPER <<<"${ERNIC_TEST_SERVER_WRAPPER:-}"

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -f "$SOCKET" "$LOG"
}
trap cleanup EXIT INT TERM

echo "=== ionic server smoke test ==="
"${SERVER_WRAPPER[@]}" "$SERVER" --backend loopback --socket "$SOCKET" \
    > "$LOG" 2>&1 &
SERVER_PID=$!

# The server creates its socket before it sets up the device, so the socket
# can appear before the banner is logged. Wait up to 5 seconds for the line
# it logs once setup is done.
READY="waiting for client connection"
for i in $(seq 1 10); do
    sleep 0.5
    if grep -q "$READY" "$LOG"; then
        echo "✓ server ready after ${i}×0.5s"
        break
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "✗ server exited early"
        cat "$LOG"
        exit 1
    fi
done

if ! grep -q "$READY" "$LOG"; then
    echo "✗ server never finished starting"
    cat "$LOG"
    exit 1
fi
if [ ! -S "$SOCKET" ]; then
    echo "✗ socket never appeared"
    cat "$LOG"
    exit 1
fi

# Verify ionic mode banner in the log
if grep -q "ionic emulation initialized" "$LOG" && \
   grep -q "VID:DID 0x1dd8:0x1002" "$LOG" && \
   grep -q "SSVID:SDID 0x1dd8:0x5400" "$LOG"; then
    echo "✓ ionic banner found in server log"
else
    echo "✗ ionic banner missing from log"
    cat "$LOG"
    exit 1
fi

echo "=== PASSED ==="
