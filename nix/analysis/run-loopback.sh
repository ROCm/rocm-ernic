# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
# Included by writeShellApplication; @...@ deadlines are internal Nix arguments.
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

# SERVER_WRAPPER retains its existing word-splitting semantics.
# shellcheck disable=SC2086
setsid $SERVER_WRAPPER "$server" --socket "$sock" --backend loopback --verbose \
  > "$OUT_LOG" 2>&1 &
spid=$!
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

kill -TERM -- "-$spid" 2>/dev/null || true
shutdown_rc=0
timeout --kill-after=1s @SHUTDOWN_TIMEOUT@ tail --sleep-interval=0.05 --pid="$spid" -f /dev/null \
  || shutdown_rc=$?
if [ "$shutdown_rc" != 0 ]; then
  kill -KILL -- "-$spid" 2>/dev/null || true
fi
server_rc=0
wait "$spid" 2>/dev/null || server_rc=$?

if [ "$ready" != 1 ]; then
  outcome='incomplete (the server socket never appeared)'
elif [ "$client_rc" != 0 ]; then
  outcome="incomplete (client failed with status $client_rc)"
elif [ "$shutdown_rc" != 0 ]; then
  outcome='incomplete (server shutdown timed out)'
elif [ "$server_rc" != 0 ]; then
  outcome="incomplete (server failed with status $server_rc)"
elif grep -q 'vfu_attach_ctx() failed' "$OUT_LOG"; then
  outcome='incomplete (the server exited on a failed client attach)'
elif ! grep -q 'Shutdown complete' "$OUT_LOG"; then
  outcome='incomplete (the server never logged a clean shutdown)'
else
  outcome='clean'
fi
echo "exercise: $outcome" | tee "$EXERCISE_FILE"
cleanup
trap - EXIT
exit 0
