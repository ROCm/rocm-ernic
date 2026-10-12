# nix/analysis/run-loopback.nix
#
# Shared exercise driver for the dynamic-analysis modules (sanitizers,
# valgrind). Starts the server on the loopback backend (no hardware),
# waits for its socket, runs the PCI-config test client against it, then
# sends SIGTERM so the server shuts down *cleanly* — the server handles
# SIGTERM via g_shutdown_requested (src/rocm_ernic_server.c), which lets
# LSan's at-exit leak check and valgrind's leak summary actually run.
#
# The leak checks only mean something if the server really reached its
# clean shutdown, so the driver also records how the run went, in
# EXERCISE_FILE, as one line:
#   exercise: clean
#   exercise: incomplete (<reason>)
# "incomplete" covers a socket that never appeared, a server that exited
# through a failed client attach, and one that never logged "Shutdown
# complete". The callers fail the build on it rather than report a leak
# check that never ran as clean.
#
# Args: $1 = server binary, $2 = client binary
# Env:
#   SERVER_WRAPPER  optional command prefix for the server (e.g. valgrind …)
#   OUT_LOG         server (and wrapper) stdout+stderr log path
#   CLIENT_LOG      client stdout+stderr log path
#   EXERCISE_FILE   where the one-line outcome is written
{ pkgs }:

# writeShellApplication (not writeShellScript): runs shellcheck at build time
# and adds a strict prologue. bashOptions is limited to nounset (`set -u`) to
# preserve this driver's "collect, never abort" behaviour — it guards the
# fallible steps itself and always exits 0.
pkgs.writeShellApplication {
  name = "run-loopback-exercise";
  runtimeInputs = [ pkgs.coreutils pkgs.gnugrep ];
  bashOptions = [ "nounset" ];
  text = ''
  server="$1"
  client="$2"
  sock="$TMPDIR/ernic-$$.sock"
  : "''${SERVER_WRAPPER:=}"
  : "''${OUT_LOG:=$TMPDIR/server.log}"
  : "''${CLIENT_LOG:=$TMPDIR/client.log}"
  : "''${EXERCISE_FILE:=$TMPDIR/exercise.txt}"

  # Start the server (optionally under a wrapper). Word-splitting of
  # SERVER_WRAPPER is intentional so valgrind's args expand.
  # shellcheck disable=SC2086
  $SERVER_WRAPPER "$server" \
    --socket "$sock" --backend loopback --verbose \
    > "$OUT_LOG" 2>&1 &
  spid=$!

  # Wait up to ~30s for the socket (valgrind-wrapped startup is slow).
  ready=0
  for _ in $(seq 1 60); do
    if [ -S "$sock" ]; then ready=1; break; fi
    kill -0 "$spid" 2>/dev/null || break
    sleep 0.5
  done

  if [ "$ready" = 1 ]; then
    sleep 1
    "$client" --socket "$sock" > "$CLIENT_LOG" 2>&1 || true
  else
    echo "run-loopback: server socket never appeared" >> "$CLIENT_LOG"
  fi

  # Graceful shutdown so leak checkers run at a clean exit.
  kill -TERM "$spid" 2>/dev/null || true
  wait "$spid" 2>/dev/null || true
  rm -f "$sock"

  if [ "$ready" != 1 ]; then
    outcome="incomplete (the server socket never appeared)"
  elif grep -q 'vfu_attach_ctx() failed' "$OUT_LOG"; then
    outcome="incomplete (the server exited on a failed client attach)"
  elif ! grep -q 'Shutdown complete' "$OUT_LOG"; then
    outcome="incomplete (the server never logged a clean shutdown)"
  else
    outcome="clean"
  fi
  echo "exercise: $outcome" | tee "$EXERCISE_FILE"
  exit 0
  '';
}
