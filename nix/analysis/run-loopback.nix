# nix/analysis/run-loopback.nix
#
# Shared exercise driver for the dynamic-analysis modules (sanitizers,
# valgrind). Starts the server on the loopback backend (no hardware),
# waits for its socket, runs the PCI-config test client against it, then
# sends SIGTERM so the server shuts down *cleanly* — the server handles
# SIGTERM via g_shutdown_requested (src/rocm_ernic_server.c), which lets
# LSan's at-exit leak check and valgrind's leak summary actually run.
#
# Args: $1 = server binary, $2 = client binary
# Env:
#   SERVER_WRAPPER  optional command prefix for the server (e.g. valgrind …)
#   OUT_LOG         server (and wrapper) stdout+stderr log path
#   CLIENT_LOG      client stdout+stderr log path
{ pkgs }:

pkgs.writeShellScript "run-loopback-exercise" ''
  set -u
  server="$1"
  client="$2"
  sock="$TMPDIR/ernic-$$.sock"
  : "''${SERVER_WRAPPER:=}"
  : "''${OUT_LOG:=$TMPDIR/server.log}"
  : "''${CLIENT_LOG:=$TMPDIR/client.log}"

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
  exit 0
''
