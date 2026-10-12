# nix/ernic-run.nix
#
# packages.ernic-run: start the rocm-ernic server, wait for its vfio-user
# socket, run a command (typically a qemu-vm.nix launcher using
# nixosModules.ernic-guest) with ERNIC_SOCKET exported, then stop the server.
#
#   ernic-run [--socket PATH] [--backend SPEC] [--stats-file F] [--log-file F] -- CMD [ARGS...]
#
# Exit status is CMD's. It is non-zero if the server never creates its
# socket, or exits before CMD finishes.

{ pkgs, rocm-ernic }:

pkgs.writeShellApplication {
  name = "ernic-run";
  runtimeInputs = [ rocm-ernic pkgs.coreutils ];
  text = ''
    usage() {
      echo 'usage: ernic-run [--socket PATH] [--backend SPEC] [--stats-file F] [--log-file F] -- CMD [ARGS...]' >&2
      exit 2
    }
    socket=""
    server_args=()
    while [ "$#" -gt 0 ]; do
      case "$1" in
        --socket) [ "$#" -ge 2 ] || usage; socket=$2; shift 2 ;;
        --backend|--stats-file|--log-file) [ "$#" -ge 2 ] || usage; server_args+=("$1" "$2"); shift 2 ;;
        --) shift; break ;;
        *) usage ;;
      esac
    done
    [ "$#" -gt 0 ] || usage
    if [ -z "$socket" ]; then
      socket_dir=$(mktemp -d -t ernic-run-XXXXXX)
      socket="$socket_dir/ernic.sock"
    fi
    if [ -e "$socket" ]; then
      echo "ernic-run: $socket already exists" >&2
      exit 1
    fi

    rocm-ernic --socket "$socket" "''${server_args[@]}" &
    server=$!
    # Stop the server however we exit; single quotes defer the expansion.
    trap 'kill -TERM "$server" 2>/dev/null || true; wait "$server" 2>/dev/null || true; rm -f "$socket"' EXIT

    # The server creates the socket in vfu_realize_ctx and then attaches
    # without blocking, so the socket appearing means QEMU may connect.
    for _ in $(seq 1 100); do
      [ -S "$socket" ] && break
      if ! kill -0 "$server" 2>/dev/null; then
        echo 'ernic-run: rocm-ernic exited before creating its socket' >&2
        exit 1
      fi
      sleep 0.1
    done
    if [ ! -S "$socket" ]; then
      echo "ernic-run: no socket at $socket after 10s" >&2
      exit 1
    fi

    export ERNIC_SOCKET="$socket"
    rc=0
    "$@" || rc=$?
    if [ "$rc" -eq 0 ] && ! kill -0 "$server" 2>/dev/null; then
      echo 'ernic-run: rocm-ernic exited while the command ran' >&2
      rc=1
    fi
    exit "$rc"
  '';
}
