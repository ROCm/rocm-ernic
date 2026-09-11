#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Dump everything worth knowing when a guest boots but never answers SSH.
#
# The interesting question in that state is which side of the host
# forward is broken: is QEMU listening at all, is the machine actually
# running, and did slirp ever hand the guest an address.  All three are
# answerable from the host, so this needs no guest login -- which is just
# as well, because a key-only guest that will not answer SSH cannot be
# logged into.
#
# Usable outside CI: the same failure is worth dumping when reproducing a
# stuck guest by hand on the self-hosted node.

set -uo pipefail

LABEL="VM"
SSH_PORT=""
QMP_SOCK=""
CONSOLE_LOG=""
QEMU_LOG=""
QEMU_PID=""

usage() {
    cat <<'EOF'
Usage: qemu-vm-diagnose.sh [options]

  --label NAME        Name to print in the banners (default: VM)
  --ssh-port PORT     Host port the guest's SSH is forwarded to
  --qmp PATH          QMP unix socket
  --console PATH      Guest serial console log
  --qemu-log PATH     QEMU's own stdout/stderr log
  --pid PID           QEMU process id

Every option is optional; each section is skipped when its input is
missing, so this can be pointed at a partially configured VM.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
    --label) LABEL="$2"; shift 2 ;;
    --ssh-port) SSH_PORT="$2"; shift 2 ;;
    --qmp) QMP_SOCK="$2"; shift 2 ;;
    --console) CONSOLE_LOG="$2"; shift 2 ;;
    --qemu-log) QEMU_LOG="$2"; shift 2 ;;
    --pid) QEMU_PID="$2"; shift 2 ;;
    -h | --help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

banner() {
    echo ""
    echo "=== ${LABEL}: $* ==="
}

# ---------------------------------------------------------------- listener

# An instant ECONNREFUSED and a five-second timeout mean opposite things:
# nothing bound versus bound but the guest never replied.  ss names the
# process, but it is not in every container image, so fall back to
# /proc/net/tcp, which is always there and answers the only question that
# actually matters.
listener_check() {
    [ -n "$SSH_PORT" ] || return 0
    banner "host listener on port ${SSH_PORT}"

    if command -v ss >/dev/null 2>&1; then
        # Just this port: the full table on a busy host buries the one
        # line being looked for.
        ss -lntp | grep -E "^State|:${SSH_PORT}([^0-9]|$)" || true
        echo ""
    fi

    python3 - "$SSH_PORT" <<'EOF'
import sys

port = int(sys.argv[1])
found = []
for path, family in (("/proc/net/tcp", "ipv4"), ("/proc/net/tcp6", "ipv6")):
    try:
        lines = open(path).read().splitlines()[1:]
    except OSError:
        continue
    for line in lines:
        f = line.split()
        # 0A is TCP_LISTEN; local_address is hex addr:port.
        if len(f) > 3 and f[3] == "0A" and int(f[1].split(":")[1], 16) == port:
            found.append((family, f[1]))

if found:
    for family, addr in found:
        print(f"listening: {family} {addr}")
else:
    print(f"NOT LISTENING: nothing is bound to port {port}")
    print("  QEMU's hostfwd never took effect, so every connection is")
    print("  refused before the guest is ever consulted.")
EOF
}

# ---------------------------------------------------------------- process

process_check() {
    [ -n "$QEMU_PID" ] || return 0
    banner "QEMU process ${QEMU_PID}"
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "alive"
        # A paused machine produces exactly the symptoms of a hung one.
        ps -o pid,stat,etime,pcpu,rss,args -p "$QEMU_PID" 2>/dev/null || true
    else
        echo "DEAD: the QEMU process is gone"
    fi
}

# -------------------------------------------------------------------- QMP

# QMP needs a capabilities handshake before it will accept commands, so
# this cannot be a one-liner with socat.  info usernet is the payoff: it
# prints slirp's redirect table host-side, without touching the guest.
qmp_check() {
    [ -n "$QMP_SOCK" ] || return 0
    [ -S "$QMP_SOCK" ] || {
        banner "QMP"
        echo "no QMP socket at ${QMP_SOCK}"
        return 0
    }
    banner "QMP ${QMP_SOCK}"

    python3 - "$QMP_SOCK" <<'EOF'
import json
import socket
import sys

sock_path = sys.argv[1]


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        self.sock.connect(path)
        self.buf = b""
        self.recv()  # greeting
        self.cmd("qmp_capabilities")

    def recv(self):
        # Events can arrive interleaved; keep reading until something
        # that is a command result or the greeting shows up.
        while True:
            while b"\n" not in self.buf:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise EOFError("QMP closed")
                self.buf += chunk
            line, self.buf = self.buf.split(b"\n", 1)
            if not line.strip():
                continue
            msg = json.loads(line)
            if "event" in msg:
                continue
            return msg

    def cmd(self, name, **args):
        req = {"execute": name}
        if args:
            req["arguments"] = args
        self.sock.sendall((json.dumps(req) + "\n").encode())
        return self.recv()


try:
    q = Qmp(sock_path)
except Exception as exc:  # noqa: BLE001 - diagnostics, report and move on
    print(f"could not talk to QMP: {exc}")
    sys.exit(0)

res = q.cmd("query-status")
status = res.get("return", {})
print(f"status: {status.get('status')} (running={status.get('running')})")
if status.get("status") in ("paused", "io-error", "internal-error"):
    print("  NOTE: a stopped machine looks exactly like a hung one from")
    print("  the outside -- this is the cause, not a symptom.")

for hmp in ("info network", "info usernet", "info block", "info chardev"):
    res = q.cmd("human-monitor-command", **{"command-line": hmp})
    out = res.get("return", res.get("error", ""))
    print(f"\n--- {hmp} ---")
    print(out if isinstance(out, str) else json.dumps(out))
EOF
}

# ------------------------------------------------------------------- logs

log_dump() {
    banner "disk space"
    df -h / /tmp 2>/dev/null || true

    if [ -n "$QEMU_LOG" ]; then
        banner "QEMU log ${QEMU_LOG}"
        if [ -f "$QEMU_LOG" ]; then
            cat "$QEMU_LOG"
        else
            echo "not found"
        fi
    fi

    if [ -n "$CONSOLE_LOG" ]; then
        # In full: a boot log is under a thousand lines, and the lines
        # that explain a network failure are near the top, which is
        # exactly what a tail throws away.
        banner "guest console ${CONSOLE_LOG} (complete)"
        if [ -f "$CONSOLE_LOG" ]; then
            wc -l "$CONSOLE_LOG"
            cat "$CONSOLE_LOG"
        else
            echo "not found"
        fi
    fi
}

listener_check
process_check
qmp_check
log_dump

echo ""
echo "=== ${LABEL}: diagnostics complete ==="
