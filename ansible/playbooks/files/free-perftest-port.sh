#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Kill whatever process is listening on the perftest data
# exchange port and wait for the socket to fully release.
# Also stops any transient systemd units left behind by
# previous perftest runs.
#
# Usage: free-perftest-port.sh [PORT] [MAX_WAIT_SEC]
#
# Idempotent -- exits 0 when the port is already free.

PORT=${1:-18515}
MAX_WAIT=${2:-10}

for unit in $(systemctl list-units --type=service \
              --state=active,running --no-legend 2>/dev/null |
              awk '/perftest|ib_(write|read|send|atomic)/ {print $1}'); do
    systemctl stop "$unit" 2>/dev/null
done

PID=$(ss -tlnp 2>/dev/null | grep ":${PORT} " |
      sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1)

if [ -z "$PID" ]; then
    exit 0
fi

kill -9 "$PID" 2>/dev/null

for i in $(seq 1 "$MAX_WAIT"); do
    ss -tlnp 2>/dev/null | grep -q ":${PORT} " || exit 0
    sleep 1
done

echo "free-perftest-port: port $PORT still held after ${MAX_WAIT}s" >&2
exit 1
