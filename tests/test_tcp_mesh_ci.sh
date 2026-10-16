#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# CI test for TCP mesh registration: GID advertisement and the protocol
# version gate. Drives real rocm-ernic processes over loopback TCP, so it
# needs no VM and no RDMA device.
#
#   1. A three-node mesh forms and every node learns every node's GIDs
#   2. A node advertising no GIDs is admitted but owns nothing
#   3. Two nodes claiming one GID leaves it owned by neither
#   4. The manager refuses a pre-v4 worker, and says why
#   5. A worker refuses a pre-v4 manager, and says why
#
# 4 and 5 matter in both directions: a mesh that admits a node running the
# old GID heuristic has a node in it that routes every destination to one
# default peer, which is the bug the advertisement exists to remove. An old
# manager cannot refuse us, so the worker has to refuse it.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
SERVER_BIN="${SERVER_BIN:-$BUILD_DIR/rocm-ernic}"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { echo -e "${GREEN}✓ $*${NC}"; }
fail() { echo -e "${RED}✗ $*${NC}"; exit 1; }
skip() { echo -e "${YELLOW}⚠ $*${NC}"; exit 77; }

if [ ! -x "$SERVER_BIN" ]; then
    skip "server binary not found: $SERVER_BIN"
fi
command -v python3 >/dev/null 2>&1 || skip "python3 not available"

RUN="/tmp/ernic-mesh-ci-$$"
mkdir -p "$RUN"
PIDS=()      # everything started in the background
NODE_PIDS=() # the mesh nodes started by start_node, which must exit 0

# An ephemeral port, so concurrent ctest jobs and a developer's own mesh do
# not collide on a fixed one.
PORT="$(python3 -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()')"

# Every mesh node must exit promptly on SIGTERM, and with status 0. A node
# still running after the grace period has hung in teardown, which fails
# the suite; it is SIGKILLed only so the job doesn't leave it behind. A
# non-zero status from a node fails the suite too: that is how a sanitizer
# report from a server process shows up here.
cleanup() {
    local rc=$? p status waited=0
    local alive=()
    for p in ${PIDS+"${PIDS[@]}"}; do
        kill "$p" 2>/dev/null || true
    done
    # Generous, for sanitizer builds; the loop ends as soon as all are gone.
    while [ $waited -lt 60 ]; do
        alive=()
        for p in ${PIDS+"${PIDS[@]}"}; do
            if kill -0 "$p" 2>/dev/null; then
                alive+=("$p")
            fi
        done
        [ ${#alive[@]} -eq 0 ] && break
        sleep 0.5; waited=$((waited + 1))
    done
    if [ ${#alive[@]} -gt 0 ]; then
        echo -e "${RED}✗ still running 30 s after SIGTERM: ${alive[*]}${NC}"
        for p in "${alive[@]}"; do
            kill -9 "$p" 2>/dev/null || true
        done
        rc=1
    fi
    for p in ${NODE_PIDS+"${NODE_PIDS[@]}"}; do
        status=0
        wait "$p" 2>/dev/null || status=$?
        if [ "$status" -ne 0 ]; then
            echo -e "${RED}✗ node $p exited with status $status${NC}"
            rc=1
        fi
    done
    if [ "$rc" -ne 0 ]; then
        for p in "$RUN"/*.log; do
            [ -f "$p" ] || continue
            echo "--- $p (last 20 lines) ---"
            tail -n 20 "$p"
        done
    fi
    rm -rf "$RUN"
    exit "$rc"
}
trap cleanup EXIT
# An interrupted run is a failed one; the exit runs cleanup() above.
trap 'exit 1' INT TERM

echo "================================================="
echo "  rocm-ernic TCP mesh registration CI tests"
echo "  Server: $SERVER_BIN"
echo "  Port:   $PORT"
echo "================================================="

# start_node <index> <manager|worker> <gid-list>
start_node() {
    local n="$1" role="$2" gids="$3" backend
    if [ "$role" = manager ]; then
        backend="tcp:manager:listen:$PORT"
    else
        backend="tcp:worker:127.0.0.1:$PORT"
    fi

    ERNIC_TCP_GUEST_GIDS="$gids" "$SERVER_BIN" \
        --socket "$RUN/$n.sock" \
        --backend "$backend" \
        --mac "$(printf '72:6f:63:6d:00:%02x' "$n")" \
        --log-level info \
        --log-file "$RUN/$n.log" &
    PIDS+=("$!")
    NODE_PIDS+=("$!")

    # Budgets here are upper bounds, not delays: every loop exits as soon
    # as its condition holds, so a generous one costs nothing on a fast
    # machine and is the difference between passing and flaking on a
    # shared runner under a sanitizer. This suite runs in all twelve build
    # legs, the slowest of which is a thread-sanitizer Release build.
    local elapsed=0
    while [ $elapsed -lt 80 ]; do
        sleep 0.5; elapsed=$((elapsed + 1))
        [ -S "$RUN/$n.sock" ] && return 0
    done
    echo "--- node $n log ---"; cat "$RUN/$n.log" 2>/dev/null
    return 1
}

# Wait until $1 appears in $2, or time out. Registration is asynchronous,
# so polling beats a fixed sleep that is either flaky or slow.
wait_for() {
    local pattern="$1" file="$2" elapsed=0
    while [ $elapsed -lt 120 ]; do
        grep -q "$pattern" "$file" 2>/dev/null && return 0
        sleep 0.5; elapsed=$((elapsed + 1))
    done
    return 1
}

GID1="fe80::706f:63ff:fe6d:1"
GID2="fe80::706f:63ff:fe6d:2"
GID3="fe80::706f:63ff:fe6d:3"

# --- Test 1: a three-node mesh forms and GIDs propagate ---
echo ""
echo "Test 1: three-node mesh converges on one GID table"
start_node 1 manager "$GID1" || fail "manager did not start"
start_node 2 worker "$GID2"  || fail "worker 2 did not start"
start_node 3 worker "$GID3"  || fail "worker 3 did not start"

wait_for "Registered node 2" "$RUN/1.log" || {
    cat "$RUN/1.log"; fail "two workers never registered"
}

for n in 1 2 3; do
    wait_for "node 2 owns GID" "$RUN/$n.log" \
        || { cat "$RUN/$n.log"; fail "node $n never learned node 2's GID"; }
    owned=$(grep -oE "node [0-9]+ owns GID [0-9a-f:]+" "$RUN/$n.log" \
            | sort -u | wc -l)
    [ "$owned" -eq 3 ] \
        || fail "node $n knows $owned of 3 GIDs"
done
pass "every node resolved all three GIDs"

# A node must know its own GID too: a QP whose destination is this node's
# own guest has to resolve locally rather than fail.
grep -q "node 0 owns GID fe80:0000:0000:0000:706f:63ff:fe6d:0001" "$RUN/1.log" \
    || fail "manager did not claim its own GID"
pass "each node claims its own GID"

# --- Test 2: a node with no GIDs configured ---
echo ""
echo "Test 2: a node advertising nothing owns nothing"
start_node 4 worker "" || fail "worker 4 did not start"
wait_for "Registered node 3" "$RUN/1.log" || fail "worker 4 never registered"
grep -q "ERNIC_TCP_GUEST_GIDS is unset" "$RUN/4.log" \
    || fail "unconfigured node did not warn"
grep -qE "node 3 owns GID" "$RUN/1.log" \
    && fail "a node that advertised nothing was given GIDs"
pass "unconfigured node registers, warns, and owns no GID"

# --- Test 3: two nodes claiming one GID ---
echo ""
echo "Test 3: a contested GID is owned by neither node"
start_node 5 worker "$GID2" || fail "worker 5 did not start"
wait_for "claimed by more than one node" "$RUN/1.log" || {
    echo "--- manager log ---"; cat "$RUN/1.log"
    echo "--- duplicate claimant log ---"; cat "$RUN/5.log"
    fail "duplicate GID was accepted silently"
}

# The original claimant must reach the same verdict. Because every node
# holds every node's CLAIMS and derives the owner itself, it detects the
# contest locally rather than being told -- so this checks convergence,
# not the notification path.
wait_for "claimed by more than one node" "$RUN/2.log" \
    || { cat "$RUN/2.log"; fail "the original claimant did not see the contest"; }
pass "duplicate GID contested on the manager and on the original claimant"

# --- Test 4: the manager refuses a pre-v4 worker ---
echo ""
echo "Test 4: manager refuses a pre-v4 worker"
# Node ids are assigned in connection order and do not track the indices
# this script uses, so "was it admitted?" is a count, not a name.
REG_BEFORE=$(grep -c "Registered node" "$RUN/1.log")
python3 - "$PORT" <<'PY' || fail "pre-v4 registration was not refused"
import socket, struct, sys

MAGIC = 0x52444D41
REGISTER_NODE, REGISTER_RESP = 12, 13

# A pre-v4 payload: hostname[256] + port + requested_node_id, no version.
body = b"oldpeer".ljust(256, b"\0") + struct.pack("!H", 9999) \
       + struct.pack("!I", 0xFFFFFFFF)
assert len(body) == 262, len(body)
hdr = struct.pack("!IIIIIIII", MAGIC, REGISTER_NODE, len(body), 1, 7, 0, 0, 0)

def read_exact(sock, n):
    """TCP delivers a stream, not messages: a single recv() can return a
    prefix of the reply and would flake this check intermittently."""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None          # EOF: short frame
        buf += chunk
    return buf


s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10)
s.sendall(hdr + body)
head = read_exact(s, 32)
if head is None:
    print("  no reply header"); s.close(); sys.exit(1)
_, msg_type, length = struct.unpack("!III", head[:12])
payload = read_exact(s, length) if length else b""
s.close()

if payload is None or len(payload) < 12:
    print(f"  short payload: want {length}, got "
          f"{0 if payload is None else len(payload)}")
    sys.exit(1)
_, _, result = struct.unpack("!III", payload[:12])
if result >= 1 << 31:
    result -= 1 << 32

if msg_type != REGISTER_RESP:
    print(f"  expected REGISTER_RESP, got {msg_type}"); sys.exit(1)
if result == 0:
    print("  manager ACCEPTED a pre-v4 worker"); sys.exit(1)
# The refusal carries the version so the old peer can report what is wanted.
if length >= 16:
    print(f"  refused with result={result}, manager version="
          f"{struct.unpack('!I', payload[12:16])[0]}")
else:
    print(f"  refused with result={result}")
PY
grep -q "refusing registration from a v0 peer" "$RUN/1.log" \
    || fail "manager refused but did not say why"
REG_AFTER=$(grep -c "Registered node" "$RUN/1.log")
[ "$REG_BEFORE" -eq "$REG_AFTER" ] \
    || fail "the refused peer was admitted anyway ($REG_BEFORE -> $REG_AFTER)"
pass "pre-v4 worker refused, with the reason logged"

# --- Test 5: a worker refuses a pre-v4 manager ---
echo ""
echo "Test 5: worker refuses a pre-v4 manager"
FAKE_PORT="$(python3 -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()')"

python3 - "$FAKE_PORT" > "$RUN/fakemgr.log" 2>&1 <<'PY' &
import socket, struct, sys, time

MAGIC = 0x52444D41
REGISTER_RESP = 13

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", int(sys.argv[1])))
srv.listen(4)
srv.settimeout(30)
conn, _ = srv.accept()
conn.recv(8192)

# Reply the way a pre-v4 manager does: 12 bytes, no version field.
body = struct.pack("!III", 1, 2, 0)
hdr = struct.pack("!IIIIIIII", MAGIC, REGISTER_RESP, len(body), 1, 0, 1, 0, 0)
conn.sendall(hdr + body)
time.sleep(3)
conn.close()
srv.close()
PY
FAKE_PID=$!
PIDS+=("$FAKE_PID")
sleep 1

ERNIC_TCP_GUEST_GIDS="fe80::9" "$SERVER_BIN" \
    --socket "$RUN/w.sock" \
    --backend "tcp:worker:127.0.0.1:$FAKE_PORT" \
    --mac 72:6f:63:6d:00:09 \
    --log-level info \
    --log-file "$RUN/w.log" &
WORKER_PID=$!
PIDS+=("$WORKER_PID")

wait_for "manager speaks v0" "$RUN/w.log" || {
    cat "$RUN/w.log" 2>/dev/null; fail "worker joined a pre-v4 manager"
}
grep -q "Registered with manager" "$RUN/w.log" \
    && fail "worker reported success against a pre-v4 manager"
pass "pre-v4 manager refused, with the reason logged"

echo ""
echo "================================================="
echo -e "${GREEN}  All TCP mesh registration tests passed${NC}"
echo "================================================="
