# shellcheck shell=bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# The body of run-loopback-exercise. run-loopback.nix replaces the @...@
# placeholders with its arguments and wraps the result with
# writeShellApplication, which supplies the shebang and `set -u`, so this
# file has neither. Usage and the outcome it records are described there.
server="$1"
client="$2"
sock="$TMPDIR/ernic-$$.sock"
: "${SERVER_WRAPPER:=}"
: "${OUT_LOG:=$TMPDIR/server.log}"
: "${CLIENT_LOG:=$TMPDIR/client.log}"
: "${EXERCISE_FILE:=$TMPDIR/exercise.txt}"
spid=""
cpid=""
cleanup() {
  if [ -n "$cpid" ]; then
    kill -KILL -- "-$cpid" 2>/dev/null || true
    wait "$cpid" 2>/dev/null || true
  fi
  if [ -n "$spid" ]; then
    kill -KILL -- "-$spid" 2>/dev/null || true
    wait "$spid" 2>/dev/null || true
  fi
  rm -f "$sock"
}
trap 'cleanup' EXIT
trap 'echo "exercise: incomplete (interrupted)" > "$EXERCISE_FILE"; exit 0' INT TERM

# The server and the client each run in a session of their own, so that a
# signal to the process group (kill -- "-$pid") reaches a wrapper such as
# valgrind and everything it started. A background job of a non-interactive
# shell isn't a process group leader, so setsid runs the command in place
# instead of forking, and $! is both its PID and its process group ID.
#
# SERVER_WRAPPER is a command prefix with its own arguments (valgrind and
# its options), so it is split into words on purpose.
# shellcheck disable=SC2086
setsid $SERVER_WRAPPER "$server" --socket "$sock" --backend loopback --verbose \
  > "$OUT_LOG" 2>&1 &
spid=$!

# Wait for the socket, but stop early if the server has already exited.
ready=0
for _ in $(seq 1 @STARTUP_ATTEMPTS@); do
  if [ -S "$sock" ]; then ready=1; break; fi
  kill -0 "$spid" 2>/dev/null || break
  sleep @POLL_SECONDS@
done

client_rc=0
if [ "$ready" = 1 ]; then
  setsid timeout --kill-after=1s @CLIENT_TIMEOUT@ "$client" --socket "$sock" \
    > "$CLIENT_LOG" 2>&1 &
  cpid=$!
  wait "$cpid" || client_rc=$?
  # A successful command must not leave background descendants behind.
  kill -KILL -- "-$cpid" 2>/dev/null || true
  cpid=""
else
  echo 'run-loopback: server socket never appeared' >> "$CLIENT_LOG"
fi

# Graceful shutdown, so the leak checkers run at a clean exit. `wait` has
# no timeout, so tail --pid waits for the server's exit under one, and
# `wait` then only collects its status.
kill -TERM -- "-$spid" 2>/dev/null || true
shutdown_rc=0
timeout --kill-after=1s @SHUTDOWN_TIMEOUT@ \
  tail --sleep-interval=0.05 --pid="$spid" -f /dev/null || shutdown_rc=$?
if [ "$shutdown_rc" != 0 ]; then
  kill -KILL -- "-$spid" 2>/dev/null || true
fi
server_rc=0
wait "$spid" 2>/dev/null || server_rc=$?

# A failed attach comes first: the server exits with an error on it, and the
# client then fails too, so either status alone would hide the cause.
if [ "$ready" != 1 ]; then
  outcome='incomplete (the server socket never appeared)'
elif grep -q 'vfu_attach_ctx() failed' "$OUT_LOG"; then
  outcome='incomplete (the server exited on a failed client attach)'
elif [ "$client_rc" != 0 ]; then
  outcome="incomplete (client failed with status $client_rc)"
elif [ "$shutdown_rc" != 0 ]; then
  outcome='incomplete (server shutdown timed out)'
elif [ "$server_rc" != 0 ]; then
  outcome="incomplete (server failed with status $server_rc)"
elif ! grep -q 'Shutdown complete' "$OUT_LOG"; then
  outcome='incomplete (the server never logged a clean shutdown)'
else
  outcome='clean'
fi
echo "exercise: $outcome" | tee "$EXERCISE_FILE"
cleanup
trap - EXIT
exit 0
