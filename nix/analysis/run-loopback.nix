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
# "incomplete" covers a socket that never appeared, a client that failed or
# timed out, a server that didn't exit in time after SIGTERM or exited
# non-zero, one that exited through a failed client attach or never logged
# "Shutdown complete", and a driver that was interrupted. The callers fail
# the build on it rather than report a leak check that never ran as clean.
#
# Args: $1 = server binary, $2 = client binary
# Env:
#   SERVER_WRAPPER  optional command prefix for the server (e.g. valgrind …)
#   OUT_LOG         server (and wrapper) stdout+stderr log path
#   CLIENT_LOG      client stdout+stderr log path
#   EXERCISE_FILE   where the one-line outcome is written
#
# The Nix arguments bound the run. The defaults leave room for a
# valgrind-wrapped server, which is slow to start; the analysis-runners
# check shortens them for its fake server and client.
#   startupAttempts  polls for the server socket before giving up
#   pollSeconds      sleep between polls (a `sleep` argument)
#   clientTimeout    limit on the client (a `timeout` duration)
#   shutdownTimeout  limit on the server's exit after SIGTERM
{
  pkgs,
  startupAttempts ? 60,
  pollSeconds ? "0.5",
  clientTimeout ? "60s",
  shutdownTimeout ? "10s",
}:

# writeShellApplication (not writeShellScript): runs shellcheck at build time
# and adds a strict prologue. bashOptions is limited to nounset (`set -u`) so
# that a failing step doesn't abort the driver before it has recorded the
# outcome and stopped the server; it guards the fallible steps itself and
# always exits 0.
pkgs.writeShellApplication {
  name = "run-loopback-exercise";
  runtimeInputs = [ pkgs.coreutils pkgs.gnugrep pkgs.util-linux ];
  bashOptions = [ "nounset" ];
  text = builtins.replaceStrings
    [ "@STARTUP_ATTEMPTS@" "@POLL_SECONDS@" "@CLIENT_TIMEOUT@" "@SHUTDOWN_TIMEOUT@" ]
    [ (toString startupAttempts) pollSeconds clientTimeout shutdownTimeout ]
    (builtins.readFile ./run-loopback.sh);
}
