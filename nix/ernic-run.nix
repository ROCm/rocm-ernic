# nix/ernic-run.nix
#
# packages.ernic-run: start the rocm-ernic server, wait for its vfio-user
# socket, run a command (typically a qemu-vm.nix launcher using
# nixosModules.ernic-guest) with ERNIC_SOCKET exported, then stop the server.
#
#   ernic-run [--socket PATH] [SERVER-OPTION VALUE]... -- CMD [ARGS...]
#
# SERVER-OPTION is one of --backend, --mac, --stats-file, --log-file,
# --tap, --nic-profile, --inject, --stats-interval-ms or --eth-queues, and
# is passed to rocm-ernic as given. --nic-profile and --inject may repeat;
# a server built without one of them rejects it at startup.
#
# Exit status is CMD's. It is non-zero if the server never creates its
# socket, or exits before CMD finishes.

{ pkgs, rocm-ernic, startupAttempts ? 100, pollSeconds ? "0.1", shutdownTimeout ? "10s" }:

pkgs.writeShellApplication {
  name = "ernic-run";
  runtimeInputs = [ rocm-ernic pkgs.coreutils pkgs.util-linux ];
  text = ''
    usage() {
      echo 'usage: ernic-run [--socket PATH] [SERVER-OPTION VALUE]... -- CMD [ARGS...]' >&2
      exit 2
    }
    socket=""
    socket_dir=""
    server=""
    command_pid=""
    stop_group() {
      local pid="$1"
      kill -TERM -- "-$pid" 2>/dev/null || true
      timeout --kill-after=1s ${shutdownTimeout} tail --sleep-interval=0.05 --pid="$pid" -f /dev/null || true
      kill -KILL -- "-$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    }
    cleanup() {
      if [ -n "$command_pid" ]; then stop_group "$command_pid"; fi
      if [ -n "$server" ]; then
        stop_group "$server"
        rm -f "$socket"
      fi
      if [ -n "$socket_dir" ]; then rmdir "$socket_dir" 2>/dev/null || true; fi
    }
    trap 'cleanup' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    server_args=()
    while [ "$#" -gt 0 ]; do
      case "$1" in
        --socket) [ "$#" -ge 2 ] || usage; socket=$2; shift 2 ;;
        --backend|--mac|--stats-file|--log-file|--tap|--nic-profile|--inject|--stats-interval-ms|--eth-queues)
          [ "$#" -ge 2 ] || usage; server_args+=("$1" "$2"); shift 2 ;;
        --) shift; break ;;
        *) usage ;;
      esac
    done
    [ "$#" -gt 0 ] || usage
    if [ -z "$socket" ]; then
      socket_dir=$(mktemp -d -t ernic-run-XXXXXX)
      socket="$socket_dir/ernic.sock"
    fi
    if [ -e "$socket" ] || [ -L "$socket" ]; then
      echo "ernic-run: $socket already exists" >&2
      exit 1
    fi

    setsid rocm-ernic --socket "$socket" "''${server_args[@]}" &
    server=$!
    # The server creates the socket in vfu_realize_ctx and then attaches
    # without blocking, so the socket appearing means QEMU may connect.
    for _ in $(seq 1 ${toString startupAttempts}); do
      [ -S "$socket" ] && break
      if ! kill -0 "$server" 2>/dev/null; then
        echo 'ernic-run: rocm-ernic exited before creating its socket' >&2
        exit 1
      fi
      sleep ${pollSeconds}
    done
    if [ ! -S "$socket" ]; then
      echo "ernic-run: no socket at $socket before the startup deadline" >&2
      exit 1
    fi

    export ERNIC_SOCKET="$socket"
    setsid "$@" <&0 &
    command_pid=$!
    while kill -0 "$command_pid" 2>/dev/null; do
      if ! kill -0 "$server" 2>/dev/null; then
        echo 'ernic-run: rocm-ernic exited while the command ran' >&2
        exit 1
      fi
      sleep ${pollSeconds}
    done
    rc=0
    wait "$command_pid" || rc=$?
    if [ "$rc" -eq 0 ] && ! kill -0 "$server" 2>/dev/null; then
      echo 'ernic-run: rocm-ernic exited while the command ran' >&2
      rc=1
    fi
    cleanup
    trap - EXIT
    exit "$rc"
  '';
}
