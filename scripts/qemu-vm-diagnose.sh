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
# It also samples where the vCPUs are, and names the known case of a guest
# kernel that hung in early boot; see "boot hang" below.
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
import re
import socket
import sys
import time

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


def hmp(command):
    res = q.cmd("human-monitor-command", **{"command-line": command})
    out = res.get("return", res.get("error", ""))
    return out if isinstance(out, str) else json.dumps(out)


def report():
    res = q.cmd("query-status")
    status = res.get("return", {})
    print(f"status: {status.get('status')} (running={status.get('running')})")
    if status.get("status") in ("paused", "io-error", "internal-error"):
        print("  NOTE: a stopped machine looks exactly like a hung one from")
        print("  the outside -- this is the cause, not a symptom.")

    for command in ("info network", "info usernet", "info block",
                    "info chardev", "info cpus"):
        print(f"\n--- {command} ---")
        print(hmp(command))

    # A guest whose kernel hung is running but going nowhere.  One register
    # dump can't tell a vCPU that's spinning from one that's stuck, so
    # sample each vCPU's RIP several times: one halted (HLT=1) is waiting
    # for an interrupt, one whose RIP moves among a few addresses is
    # spinning, most likely waiting on another vCPU.  The addresses are
    # shifted by KASLR, so compare them with each other, not with
    # System.map.
    samples = 5
    print(f"\n--- vCPU RIP, {samples} samples 0.5 s apart ---")
    regs = ""
    for n in range(samples):
        if n:
            time.sleep(0.5)
        regs = hmp("info registers -a")
        cpus = re.findall(r"CPU#(\d+).*?RIP=([0-9a-f]+).*?HLT=(\d)", regs,
                          re.S)
        if not cpus:
            print("could not parse info registers -a; full output follows")
            break
        print(f"{n}: " + "  ".join(f"#{c} {rip} hlt={h}"
                                   for c, rip, h in cpus))

    print("\n--- info registers -a (last sample) ---")
    print(regs)


# QEMU answers QMP from its main loop, and reading a vCPU's registers
# has to wait for that vCPU.  A vCPU stuck inside QEMU, such as one
# waiting for a device to answer an access, can hold either up, and then
# a command never returns.  That is a finding in itself, so report it
# rather than dying with a traceback.
try:
    report()
except (OSError, EOFError) as exc:
    print(f"\nQMP stopped answering: {exc}")
    print("  QEMU itself may be stuck, rather than the guest: for example, a")
    print("  vCPU waiting for a device to answer an access.  That includes")
    print("  the emulated NIC, so check the server log for whether it")
    print("  answered the guest's last access.")
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

# -------------------------------------------------------------- boot hang

# The hosted runners are themselves VMs, so a CI guest runs nested. Now and
# then (three times in about 170 System Tests runs, 2026-10-01 to
# 2026-10-20, in the loopback, S3, and TCP mesh jobs alike) the guest kernel
# stops right after "hpet0: 3 comparators", before "clocksource: Switched to
# clocksource kvm-clock", and stays there with every vCPU busy. That's
# before the ionic driver loads, and the server answered every config space
# access the guest made, so it isn't the emulator.
#
# The hosted workflows start guests with -machine hpet=off, to see whether
# taking the HPET away avoids the hang. If this still reports a hang, it
# didn't, and the next step is to stop waiting the hang out: a healthy guest
# answers SSH within about 30 s, so the boot wait can reset a guest whose
# console has stopped growing short of the clocksource switch (QMP
# system_reset), once, with a ::warning::, and fail if it hangs again. That
# is a retry, which AGENTS.md otherwise rules out for a flaky test. It's
# justified only for this hang, which is in the guest kernel, so the reset
# must not cover anything else that stops a guest at the same point (see
# below). In particular, it mustn't reset a guest whose server stopped
# answering: check that the server is still serving before resetting.
#
# Stopping before the clocksource switch is not enough to say it's this
# hang. A panic, a paused machine, or a device access that never completed
# looks the same, and by then the guest has made dozens of config space
# accesses to the emulated NIC, each of which a vCPU waits on. So this
# reports what it saw and what else to rule out, not a verdict.
boot_hang_check() {
    local last failure

    [ -n "$CONSOLE_LOG" ] && [ -f "$CONSOLE_LOG" ] || return 0
    grep -q 'Linux version' "$CONSOLE_LOG" || return 0
    grep -q 'clocksource: Switched to clocksource' "$CONSOLE_LOG" && return 0

    banner "the guest kernel stopped before its clocksource switch"

    # The serial console's lines end in \r\n.
    failure=$(grep -m 1 -E 'Kernel panic|BUG:|Oops' "$CONSOLE_LOG" | tr -d '\r')
    if [ -n "$failure" ]; then
        echo "The kernel reported a failure; see the console above:"
        echo "  ${failure}"
        echo "This is not the nested-guest hang described under \"boot hang\""
        echo "in scripts/qemu-vm-diagnose.sh."
        annotate "guest kernel failed during boot" "${failure}"
        return 0
    fi

    last=$(grep -E '^\[ *[0-9]+\.[0-9]+\]' "$CONSOLE_LOG" | tail -n 1 |
        tr -d '\r')
    echo "The kernel started but never switched to its final clocksource."
    echo "Its last message was:"
    echo "  ${last:-(none with a timestamp)}"
    echo ""
    echo "That fits the nested-guest hang described under \"boot hang\" in"
    echo "scripts/qemu-vm-diagnose.sh, whose last message so far has been"
    echo "\"hpet0: 3 comparators\" (a guest with hpet=off doesn't print it)."
    echo "But a paused machine, or a device access that never completed,"
    echo "including one to the emulated NIC, looks the same. Before"
    echo "putting it down to that hang, check the QMP status and vCPU"
    echo "samples above, and that the server log shows it answered the"
    echo "guest's last access."
    annotate "guest kernel stopped before its clocksource switch" \
        "Last message: ${last:-none}. See this step's log before calling it" \
        "the known nested-guest hang (\"boot hang\" in" \
        "scripts/qemu-vm-diagnose.sh)."
}

# A GitHub Actions error annotation, which shows on the run's summary page:
# annotate TITLE WORDS..., with the words joined by spaces. The message's %
# must be escaped, and the title has no : or , to escape.
annotate() {
    local title="$1" message
    shift
    message="$*"
    [ -n "${GITHUB_ACTIONS:-}" ] || return 0
    echo "::error title=${title} (${LABEL})::${message//%/%25}"
}

listener_check
process_check
qmp_check
log_dump
boot_hang_check

echo ""
echo "=== ${LABEL}: diagnostics complete ==="
