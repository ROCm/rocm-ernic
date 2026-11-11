#!/usr/bin/env bash
#
# ci/boot-guest.sh, and ernicctl vm-launch and vm-stop through it, against
# stand-ins for QEMU, ssh, cloud-localds and qemu-img: the QEMU command
# line it builds, the per-boot key and the seed that authorizes it, the
# wait for SSH and what happens when QEMU dies or the guest never answers,
# the arguments it refuses, and what ernicctl records in its manifest.
#
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Usage: test_boot_guest.sh BOOT_GUEST ERNICCTL PYTHON
#
# Exit 0 = pass, exit 1 = fail.

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "Usage: $0 BOOT_GUEST ERNICCTL PYTHON" >&2
    exit 1
fi
HELPER=$1
ERNICCTL=$2
PYTHON=$3

WORK=$(mktemp -d "${TMPDIR:-/tmp}/test-boot-guest-XXXXXX")
BIN="$WORK/bin"
SERVER_PID=""
LOCK_PID=""
ZOMBIE_PARENT_PID=""
LAUNCH_PID=""

cleanup() {
    local pid
    if [[ -f $WORK/qemu-pids ]]; then
        while read -r pid; do
            kill "$pid" 2>/dev/null || true
        done <"$WORK/qemu-pids"
    fi
    for pid in "$SERVER_PID" "$LOCK_PID" "$ZOMBIE_PARENT_PID" "$LAUNCH_PID"; do
        if [[ -n $pid ]]; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

pass() {
    echo "PASS: $*"
}

for tool in ssh-keygen setsid timeout; do
    if ! command -v "$tool" >/dev/null; then
        fail "$tool is needed and not installed"
    fi
done

# ── Stand-ins ───────────────────────────────────

mkdir -p "$BIN"

# QEMU: answers the helper's probes, then records its arguments, one to a
# line, and runs until it's killed, as a booted guest would
cat >"$BIN/qemu-system-x86_64" <<'EOF'
#!/usr/bin/env bash
case " $* " in
    *" -device help "*)
        echo 'name "pci-mmio-bridge", bus PCI'
        exit 0
        ;;
    *" -device nvme,help "*)
        printf '  ioeventfd=<bool>\n  dbcs=<bool>\n'
        exit 0
        ;;
    *" -machine none "*)
        exit 0
        ;;
esac
echo $$ >>"$FAKE_QEMU_PIDS"
if [[ -n ${FAKE_QEMU_FAIL:-} ]]; then
    echo "fake QEMU refused to start" >&2
    exit 3
fi
printf '%s\n' "$@" >"$FAKE_QEMU_ARGS.tmp"
mv "$FAKE_QEMU_ARGS.tmp" "$FAKE_QEMU_ARGS"
exec sleep 300
EOF

# ssh to a guest that is up once QEMU has started, and trusts only the
# keys its seed's user-data authorizes for the user logging in.
# FAKE_SSH_HANG makes it answer, successfully, only after 30 seconds.
cat >"$BIN/ssh" <<'EOF'
#!/usr/bin/env bash
key=""
port=""
while [[ $# -gt 0 ]]; do
    case $1 in
        -i) key=$2; shift 2 ;;
        -p) port=$2; shift 2 ;;
        -F | -o) shift 2 ;;
        -*) shift ;;
        *) break ;;
    esac
done
echo "$port $*" >>"$FAKE_SSH_LOG"
if [[ -n ${FAKE_SSH_HANG:-} ]]; then
    exec sleep 30
fi
if [[ -n ${FAKE_SSH_FAIL:-} || ! -s $FAKE_QEMU_ARGS ]]; then
    echo "Connection reset by 127.0.0.1 port $port" >&2
    exit 255
fi
if [[ $1 != *@127.0.0.1 ]]; then
    echo "not the forwarded port's address: $1" >&2
    exit 255
fi
user_data="$(dirname "$key")/user-data"
if ! grep -qx -- "  - name: '${1%@*}'" "$user_data" ||
    ! grep -qF -- "$(cut -d' ' -f1,2 "$key.pub")" "$user_data"; then
    echo "Permission denied (publickey)." >&2
    exit 255
fi
EOF

cat >"$BIN/cloud-localds" <<'EOF'
#!/usr/bin/env bash
cat "$2" "$3" >"$1"
EOF

# qemu-img create: makes the file, and records its name
cat >"$BIN/qemu-img" <<'EOF'
#!/usr/bin/env bash
[[ $1 == create ]] || exit 1
shift
while [[ $# -gt 0 ]]; do
    case $1 in
        -f | -F | -b | -o) shift 2 ;;
        -q) shift ;;
        *) break ;;
    esac
done
echo "$1" >>"$FAKE_QEMU_IMG_LOG"
: >"$1"
EOF

chmod +x "$BIN"/*
export PATH="$BIN:$PATH"
export FAKE_QEMU_PIDS="$WORK/qemu-pids"
export FAKE_SSH_LOG="$WORK/ssh.log"
export FAKE_QEMU_IMG_LOG="$WORK/qemu-img.log"
QEMU="$BIN/qemu-system-x86_64"

# ── Fixtures ────────────────────────────────────

IMAGES="$WORK/images"
mkdir -p "$IMAGES"
DISK="$IMAGES/guest.qcow2"
: >"$DISK"

# A vfio-user socket for QEMU to find; nothing need listen on it
SOCK="$WORK/vfu.sock"
"$PYTHON" -I -c 'import socket, sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])' \
    "$SOCK"

# The user's own key
USER_KEY="$WORK/user_key"
ssh-keygen -q -t ed25519 -N '' -C "user's key" -f "$USER_KEY" </dev/null

# Check that a process is running. A zombie isn't: under an init that
# doesn't reap, as in a container job, a killed QEMU whose parent has
# exited stays one, and kill -0 still finds it.
#   $1  the PID
running() {
    local stat

    stat=$(cat "/proc/$1/stat" 2>/dev/null) || return 1
    [[ ${stat##*) } != Z* ]]
}

# Check that a QEMU argument is on the command line.
#   $1  the recorded arguments
#   $2  the argument
has_arg() {
    if ! grep -qFx -- "$2" "$1"; then
        echo "QEMU's arguments were:" >&2
        cat "$1" >&2
        fail "QEMU wasn't given $2"
    fi
}

# Print the value of KEY in k=v output.
#   $1  the output
#   $2  KEY
value() {
    sed -n "s/^$2=//p" "$1"
}

# ── A boot with every option ────────────────────

export FAKE_QEMU_ARGS="$WORK/qemu-args-1"
OUT="$WORK/vm1"
"$HELPER" --disk "$DISK" --name test-vm --ssh-port 2299 --out "$OUT" \
    --qemu "$QEMU" --accel tcg --cpus 2 --mem 512 --timeout 30 \
    --qmp "$WORK/qmp.sock" --qga "$WORK/qga.sock" \
    --vfio-user "$SOCK" --vfio-user-id ernic1 \
    --mcast 230.0.0.1:1234 --hostfwd tcp:127.0.0.1:9150-:9100 \
    --nvme 2 --pci-mmio-bridge --authorize "$USER_KEY.pub" \
    --snapshot --shared-backing -- -name extra \
    >"$WORK/out-1" 2>"$WORK/err-1" ||
    fail "a boot with every option failed: $(cat "$WORK/err-1")"

PID=$(value "$WORK/out-1" pid)
KEY=$(value "$WORK/out-1" key)
[[ $KEY == "$OUT/id_ed25519" ]] || fail "key= is '$KEY'"
[[ $(value "$WORK/out-1" user) == ubuntu ]] || fail "user= isn't ubuntu"
[[ $(value "$WORK/out-1" port) == 2299 ]] || fail "port= isn't 2299"
running "$PID" || fail "pid= $PID isn't running"
grep -qx "$PID" "$FAKE_QEMU_PIDS" || fail "pid= $PID isn't QEMU's"
[[ $(stat -c %a "$OUT") == 700 ]] || fail "--out isn't mode 0700"
[[ $(stat -c %a "$KEY") == 600 ]] || fail "the key isn't mode 0600"
pass "the helper reports QEMU's PID, the key, the user and the port"

A="$FAKE_QEMU_ARGS"
has_arg "$A" "q35,accel=tcg,memory-backend=mem0,hpet=off"
has_arg "$A" "qemu64"
has_arg "$A" "memory-backend-memfd,id=mem0,share=on,size=512M"
has_arg "$A" "file=$DISK,format=qcow2,if=virtio,backing.file.locking=off"
has_arg "$A" "file=$OUT/seed.img,format=raw,if=virtio,readonly=on"
has_arg "$A" "user,id=net0,hostfwd=tcp:127.0.0.1:2299-:22,hostfwd=tcp:127.0.0.1:9150-:9100"
has_arg "$A" "pcie-root-port,id=pcie-vfu.1,chassis=1"
has_arg "$A" "{\"driver\":\"vfio-user-pci\",\"id\":\"ernic1\",\"bus\":\"pcie-vfu.1\",\"socket\":{\"type\":\"unix\",\"path\":\"$SOCK\"}}"
has_arg "$A" "unix:$WORK/qmp.sock,server=on,wait=off"
has_arg "$A" "socket,id=qga0,path=$WORK/qga.sock,server=on,wait=off"
has_arg "$A" "socket,id=net1,mcast=230.0.0.1:1234"
# 2299 is 0x08fb
has_arg "$A" "virtio-net-pci,netdev=net1,mac=52:54:00:00:08:fb"
has_arg "$A" "file:$OUT/console.log"
has_arg "$A" "-snapshot"
pass "the command line has the machine, disks, network and devices asked for"

for i in 1 2; do
    has_arg "$A" "file=$IMAGES/test-vm-nvme$i.qcow2,format=qcow2,if=none,id=nvme-$i,aio=io_uring,cache.direct=on,discard=unmap,detect-zeroes=unmap"
    has_arg "$A" "nvme,serial=test-vm-nvme$i,id=nvme-$i-dev,ioeventfd=off,dbcs=off"
    has_arg "$A" "nvme-ns,drive=nvme-$i,bus=nvme-$i-dev,nsid=1"
    [[ -f $IMAGES/test-vm-nvme$i.qcow2 ]] || fail "NVMe image $i wasn't made"
done
bridge=$(grep -n '^pci-mmio-bridge,' "$A" | cut -d: -f1)
first_nvme=$(grep -n '^file=.*-nvme1\.qcow2,' "$A" | cut -d: -f1)
[[ -n $bridge && $bridge -lt $first_nvme ]] ||
    fail "the MMIO bridge isn't on the command line before the NVMe devices"
pass "NVMe images are made, with the probed AIO, behind the MMIO bridge"

[[ $(tail -n 2 "$A" | tr '\n' ' ') == "-name extra " ]] ||
    fail "the arguments after -- aren't last"
pass "arguments after -- go to QEMU last"

grep -qF -- "$(cut -d' ' -f1,2 "$KEY.pub")" "$OUT/user-data" ||
    fail "user-data doesn't authorize the boot's key"
grep -qF -- "$(cut -d' ' -f1,2 "$USER_KEY.pub")" "$OUT/user-data" ||
    fail "user-data doesn't authorize --authorize's key"
grep -qx 'hostname: test-vm' "$OUT/user-data" ||
    fail "user-data doesn't set the hostname"
grep -qx '  - default' "$OUT/user-data" ||
    fail "user-data drops the image's default user"
grep -qx "  - name: 'ubuntu'" "$OUT/user-data" ||
    fail "user-data doesn't authorize the keys for --user"
grep -qx 'instance-id: test-vm-[0-9a-f]\{16\}' "$OUT/meta-data" ||
    fail "meta-data has no instance-id"
[[ -s $OUT/seed.img ]] || fail "no seed was made"
pass "the seed authorizes the boot's key and the user's"

grep -q '^2299 ubuntu@127.0.0.1 true$' "$FAKE_SSH_LOG" ||
    fail "the helper didn't wait for ssh to ubuntu@127.0.0.1:2299"
pass "the helper waits until the guest accepts the key"

kill "$PID"
first_key=$(cat "$KEY.pub")
first_id=$(cat "$OUT/meta-data")

# ── Every boot has its own key, for --user ──────

export FAKE_QEMU_ARGS="$WORK/qemu-args-2"
"$HELPER" --disk "$DISK" --name test-vm --ssh-port 2299 --out "$OUT" \
    --qemu "$QEMU" --accel kvm --timeout 30 --user admin \
    >"$WORK/out-2" 2>"$WORK/err-2" ||
    fail "a second boot failed: $(cat "$WORK/err-2")"
kill "$(value "$WORK/out-2" pid)"
[[ $(value "$WORK/out-2" user) == admin ]] || fail "user= isn't admin"
grep -qx "  - name: 'admin'" "$OUT/user-data" ||
    fail "user-data doesn't authorize the keys for --user admin"
grep -q '^2299 admin@127.0.0.1 true$' "$FAKE_SSH_LOG" ||
    fail "the helper didn't log in as admin"
[[ $(cat "$KEY.pub") != "$first_key" ]] || fail "the second boot reused the key"
[[ $(cat "$OUT/meta-data") != "$first_id" ]] ||
    fail "the second boot reused the instance-id"
has_arg "$FAKE_QEMU_ARGS" "host"
has_arg "$FAKE_QEMU_ARGS" "file=$DISK,format=qcow2,if=virtio"
pass "each boot has a new key and instance-id, authorized for --user"

# ── Failures stop QEMU ──────────────────────────

export FAKE_QEMU_ARGS="$WORK/qemu-args-3"
cp "$FAKE_QEMU_PIDS" "$FAKE_QEMU_PIDS.before"
if FAKE_SSH_FAIL=1 "$HELPER" --disk "$DISK" --name test-vm --ssh-port 2299 \
    --out "$WORK/vm3" --qemu "$QEMU" --timeout 3 \
    >"$WORK/out-3" 2>"$WORK/err-3"; then
    fail "the helper succeeded though the guest never accepted SSH"
fi
grep -q "didn't accept SSH within 3s" "$WORK/err-3" ||
    fail "the timeout wasn't reported: $(cat "$WORK/err-3")"
grep -q 'Connection reset' "$WORK/err-3" ||
    fail "ssh's error wasn't reported"
[[ ! -s $WORK/out-3 ]] || fail "a failed boot printed results"
PID=$(comm -13 <(sort "$FAKE_QEMU_PIDS.before") <(sort "$FAKE_QEMU_PIDS"))
[[ -n $PID ]] || fail "QEMU didn't start"
! running "$PID" || fail "QEMU was left running after a timeout"
pass "a guest that never accepts SSH times out, and QEMU is stopped"

# A probe would answer, successfully, long after the wait ends
export FAKE_QEMU_ARGS="$WORK/qemu-args-hang"
start=$SECONDS
if FAKE_SSH_HANG=1 "$HELPER" --disk "$DISK" --name test-vm --ssh-port 2299 \
    --out "$WORK/vm-hang" --qemu "$QEMU" --timeout 3 \
    >"$WORK/out-hang" 2>"$WORK/err-hang"; then
    fail "the helper took an answer that came after its timeout"
fi
((SECONDS - start < 15)) ||
    fail "the helper waited $((SECONDS - start))s with a 3s timeout"
grep -q "didn't accept SSH within 3s" "$WORK/err-hang" ||
    fail "the timeout wasn't reported: $(cat "$WORK/err-hang")"
pass "an SSH probe is cut off when the wait ends"

export FAKE_QEMU_ARGS="$WORK/qemu-args-4"
if FAKE_QEMU_FAIL=1 "$HELPER" --disk "$DISK" --name test-vm \
    --ssh-port 2299 --out "$WORK/vm4" --qemu "$QEMU" --timeout 30 \
    >"$WORK/out-4" 2>"$WORK/err-4"; then
    fail "the helper succeeded though QEMU exited"
fi
grep -q 'QEMU exited with status 3' "$WORK/err-4" ||
    fail "QEMU's exit wasn't reported: $(cat "$WORK/err-4")"
grep -q 'fake QEMU refused to start' "$WORK/err-4" ||
    fail "QEMU's log wasn't shown"
pass "QEMU exiting before the guest is up is reported with its log"

# ── Refused arguments ───────────────────────────

: >"$WORK/a,b.qcow2"
: >"$WORK/qmp-file"
printf 'not a key\n' >"$WORK/bad.pub"

# Check that the helper refuses arguments, and starts nothing.
#   $1  what it should say
#   $@  the arguments, after the required ones
refuses() {
    local want=$1
    shift
    rm -f "$WORK/qemu-args-refused"
    if FAKE_QEMU_ARGS="$WORK/qemu-args-refused" "$HELPER" \
        --disk "$DISK" --name test-vm --ssh-port 2299 --out "$WORK/vm5" \
        --qemu "$QEMU" --timeout 5 "$@" >/dev/null 2>"$WORK/err-5"; then
        fail "the helper accepted $*"
    fi
    grep -qF -- "$want" "$WORK/err-5" ||
        fail "for $*, the helper said '$(cat "$WORK/err-5")', not '$want'"
    [[ ! -e $WORK/qemu-args-refused ]] || fail "QEMU started for $*"
}

refuses "--name must be a hostname" --name bad_name
refuses "--disk can't contain a comma" --disk "$WORK/a,b.qcow2"
refuses "--disk isn't a readable file" --disk "$WORK/missing.qcow2"
refuses "--ssh-port must be from 1 to 65535" --ssh-port 0
refuses "--ssh-port isn't a number" --ssh-port 022
refuses "--accel must be kvm or tcg" --accel xen
refuses "--hostfwd must be" --hostfwd "tcp::1-:2,guestfwd=tcp:10.0.2.9:1-cmd:sh"
refuses "--vfio-user isn't a socket" --vfio-user "$WORK/missing.sock"
refuses "--vfio-user-id isn't a QEMU ID" --vfio-user-id '1bad'
refuses "--qmp exists and isn't a socket" --qmp "$WORK/qmp-file"
refuses "--mcast must be an IPv4 ADDR:PORT" --mcast 230.0.0.1
refuses "--pci-hostdev isn't a PCI address" --pci-hostdev zz:00.0
refuses "isn't bound to vfio-pci" --pci-hostdev 0000:ff:1f.7
refuses "--nvme must be from 0 to 32" --nvme 33
refuses "--nvme-aio must be" --nvme 1 --nvme-aio fast
refuses "holds something other than SSH public keys" --authorize "$WORK/bad.pub"
refuses "--disk needs a value" --disk
refuses "unknown option: --bogus" --bogus
pass "malformed arguments are refused before QEMU starts"

# ── ernicctl vm-launch and vm-stop ──────────────

sleep 300 &
SERVER_PID=$!

export ERNIC_RUN_DIR="$WORK/run"
export ERNIC_LOG_DIR="$WORK/log"
# Not made here: vm-launch makes it for the overlay
export ERNIC_VM_IMAGE_DIR="$WORK/vm-images/new"
export ERNIC_VM_BACKING="$DISK"
export ERNIC_QEMU_PATH="$BIN"
export ERNIC_VM_NAME=ernic-test
export ERNIC_VM_SSH_BASE_PORT=2300
export ERNIC_VM_SSH_USER=ubuntu
export ERNIC_VM_SSH_HOST=localhost
export ERNIC_VM_SSH_IDENTITY="$USER_KEY"
export ERNIC_VM_VCPUS=1
export ERNIC_VM_MEM=512
export ERNIC_VM_MCAST_GROUP=""
export ERNIC_SOCKET_GROUP=""
export KVM=disable
unset SUDO_USER
mkdir -p "$ERNIC_RUN_DIR" "$ERNIC_LOG_DIR"
MANIFEST="$ERNIC_RUN_DIR/instances.json"
cat >"$MANIFEST" <<EOF
{"instances": [{"id": 1, "role": "manager", "pid": $SERVER_PID,
  "mac": "02:00:00:00:00:01", "socket": "$SOCK", "vm": null}]}
EOF

# Print a field of instance 1's VM in the manifest.
#   $1  the field
vm_field() {
    "$PYTHON" -I -c '
import json, sys
vm = json.load(open(sys.argv[1]))["instances"][0]["vm"]
print("" if vm is None else vm[sys.argv[2]])
' "$MANIFEST" "$1"
}

# Another launch for instance 1, booting
"$PYTHON" -I -c '
import fcntl, os, sys, time
fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o600)
fcntl.flock(fd, fcntl.LOCK_EX)
open(sys.argv[2], "w").close()
time.sleep(300)
' "$ERNIC_RUN_DIR/vm-1.lock" "$WORK/locked" &
LOCK_PID=$!
deadline=$((SECONDS + 30))
until [[ -e $WORK/locked ]]; do
    ((SECONDS < deadline)) || fail "the stand-in launch didn't take the lock"
    sleep 0.1
done
if "$PYTHON" -I "$ERNICCTL" vm-launch 1 --boot-timeout 30 \
    >"$WORK/out-locked" 2>"$WORK/err-locked"; then
    fail "vm-launch ran while another launch for instance 1 was booting"
fi
grep -q 'Another vm-launch or vm-stop for instance 1 is running' "$WORK/err-locked" ||
    fail "a concurrent vm-launch wasn't refused: $(cat "$WORK/err-locked")"
[[ ! -e $ERNIC_RUN_DIR/vm-1 ]] || fail "a refused vm-launch ran the helper"
kill "$LOCK_PID"
wait "$LOCK_PID" || true
LOCK_PID=""
pass "vm-launch refuses an instance another launch is booting"

# Start vm-launch 1 in the background, against a guest that never answers,
# and wait until its QEMU has started.
#   $1  a name for its files
#   $@  the environment for it, as NAME=VALUE
start_launch() {
    local name=$1 deadline
    shift
    export FAKE_QEMU_ARGS="$WORK/qemu-args-$name"
    cp "$FAKE_QEMU_PIDS" "$FAKE_QEMU_PIDS.before"
    env "$@" "$PYTHON" -I "$ERNICCTL" vm-launch 1 --boot-timeout 120 \
        >"$WORK/out-$name" 2>"$WORK/err-$name" &
    LAUNCH_PID=$!
    deadline=$((SECONDS + 30))
    until [[ -s $FAKE_QEMU_ARGS ]]; do
        ((SECONDS < deadline)) || fail "$name: vm-launch didn't start QEMU"
        sleep 0.1
    done
    LAUNCH_QEMU_PID=$(comm -13 <(sort "$FAKE_QEMU_PIDS.before") \
        <(sort "$FAKE_QEMU_PIDS"))
}

# Wait for the background vm-launch, which must fail, saying why.
#   $1  the name start_launch() had
#   $2  what it must say
end_launch() {
    local status=0
    wait "$LAUNCH_PID" || status=$?
    LAUNCH_PID=""
    ((status != 0)) || fail "$1: vm-launch succeeded though it was stopped"
    grep -qF -- "$2" "$WORK/err-$1" ||
        fail "$1: vm-launch said '$(cat "$WORK/err-$1")', not '$2'"
    ! running "$LAUNCH_QEMU_PID" || fail "$1: QEMU was left running"
    [[ -z $(vm_field vm_pid) ]] || fail "$1: a stopped launch was recorded"
}

# Each probe hangs, so this also checks the helper doesn't wait one out
start_launch cancel FAKE_SSH_HANG=1
start=$SECONDS
"$PYTHON" -I "$ERNICCTL" vm-stop 1 >"$WORK/out-stop" 2>"$WORK/err-stop" ||
    fail "vm-stop of a booting launch failed: $(cat "$WORK/err-stop")"
grep -q 'Cancelling the vm-launch for instance 1' "$WORK/out-stop" ||
    fail "vm-stop didn't cancel the launch: $(cat "$WORK/out-stop")"
end_launch cancel "cancelled by vm-stop"
((SECONDS - start < 20)) ||
    fail "cancelling the launch took $((SECONDS - start))s"
[[ ! -e $ERNIC_RUN_DIR/vm-1.cancel ]] || fail "vm-stop left its cancel file"
pass "vm-stop cancels a launch that is still booting, and its QEMU"

start_launch term FAKE_SSH_FAIL=1
kill -TERM "$LAUNCH_PID"
end_launch term "stopped by SIGTERM"
pass "a vm-launch stopped by SIGTERM stops its QEMU"

# A helper whose guest is up, and which is stopped before it exits: the
# helper leaves QEMU running then, so the launch must stop it. ernicctl
# runs the helper beside it, so a copy runs with a stand-in.
mkdir -p "$WORK/tree/service" "$WORK/tree/ci"
cp "$ERNICCTL" "$WORK/tree/service/ernicctl"
cat >"$WORK/tree/ci/boot-guest.sh" <<'END_HELPER'
#!/usr/bin/env bash
sleep 300 </dev/null >/dev/null 2>&1 &
qemu=$!
sleep 300 </dev/null >/dev/null 2>&1 &
idle=$!
trap 'kill "$idle"; exit 143' TERM
printf 'pid=%s\nkey=/dev/null\nuser=ubuntu\nport=2300\n' "$qemu"
echo "$qemu" >>"$FAKE_QEMU_PIDS"
wait "$idle"
END_HELPER
chmod +x "$WORK/tree/ci/boot-guest.sh"
cp "$FAKE_QEMU_PIDS" "$FAKE_QEMU_PIDS.before"
"$PYTHON" -I "$WORK/tree/service/ernicctl" vm-launch 1 --boot-timeout 120 \
    >"$WORK/out-late" 2>"$WORK/err-late" &
LAUNCH_PID=$!
deadline=$((SECONDS + 30))
until LAUNCH_QEMU_PID=$(comm -13 <(sort "$FAKE_QEMU_PIDS.before") \
    <(sort "$FAKE_QEMU_PIDS")) && [[ -n $LAUNCH_QEMU_PID ]]; do
    ((SECONDS < deadline)) || fail "late: the stand-in helper didn't start"
    sleep 0.1
done
"$PYTHON" -I "$ERNICCTL" vm-stop 1 >"$WORK/out-stop-late" \
    2>"$WORK/err-stop-late" ||
    fail "vm-stop of a launch whose guest was up failed"
end_launch late "cancelled by vm-stop"
pass "a launch stopped just after its guest came up stops its QEMU"

export FAKE_QEMU_ARGS="$WORK/qemu-args-6"
"$PYTHON" -I "$ERNICCTL" vm-launch 1 --boot-timeout 30 \
    --extra-hostfwd ,hostfwd=tcp:127.0.0.1:9150-:9100 \
    >"$WORK/out-6" 2>"$WORK/err-6" ||
    fail "ernicctl vm-launch failed: $(cat "$WORK/out-6" "$WORK/err-6")"

[[ -f $ERNIC_VM_IMAGE_DIR/ernic-test-1.qcow2 ]] ||
    fail "vm-launch didn't make instance 1's overlay, and its directory"
PID=$(vm_field vm_pid)
running "$PID" || fail "the manifest's vm_pid $PID isn't running"
grep -qx "$PID" "$FAKE_QEMU_PIDS" || fail "the manifest's vm_pid isn't QEMU's"
[[ $(vm_field vm_name) == ernic-test-1 ]] || fail "vm_name isn't ernic-test-1"
[[ $(vm_field ssh_port) == 2300 ]] || fail "ssh_port isn't 2300"
[[ $(vm_field device_id) == ernic1 ]] || fail "device_id isn't ernic1"
[[ $(vm_field ssh_identity) == "$ERNIC_RUN_DIR/vm-1/id_ed25519" ]] ||
    fail "ssh_identity isn't the boot's key"
A="$FAKE_QEMU_ARGS"
has_arg "$A" "q35,accel=tcg,memory-backend=mem0,hpet=off"
has_arg "$A" "file=$ERNIC_VM_IMAGE_DIR/ernic-test-1.qcow2,format=qcow2,if=virtio,backing.file.locking=off"
has_arg "$A" "user,id=net0,hostfwd=tcp:127.0.0.1:2300-:22,hostfwd=tcp:127.0.0.1:9150-:9100"
has_arg "$A" "{\"driver\":\"vfio-user-pci\",\"id\":\"ernic1\",\"bus\":\"pcie-vfu.1\",\"socket\":{\"type\":\"unix\",\"path\":\"$SOCK\"}}"
has_arg "$A" "unix:$ERNIC_RUN_DIR/qmp-1.sock,server=on,wait=off"
has_arg "$A" "file:$ERNIC_LOG_DIR/vm-1.log"
grep -qF -- "$(cut -d' ' -f1,2 "$USER_KEY.pub")" "$ERNIC_RUN_DIR/vm-1/user-data" ||
    fail "vm-launch didn't authorize ERNIC_VM_SSH_IDENTITY's key"
grep -q '^SSH: ssh -p 2300 ubuntu@localhost$' "$WORK/out-6" ||
    fail "vm-launch didn't say how to log in: $(cat "$WORK/out-6")"
pass "vm-launch boots instance 1's guest and records it"

if "$PYTHON" -I "$ERNICCTL" vm-launch 1 --boot-timeout 30 \
    >"$WORK/out-7" 2>"$WORK/err-7"; then
    fail "vm-launch started a second VM for instance 1"
fi
grep -q 'already has a VM' "$WORK/err-7" ||
    fail "a second vm-launch wasn't refused: $(cat "$WORK/err-7")"
pass "vm-launch refuses an instance whose VM is running"

if "$PYTHON" -I "$ERNICCTL" vm-launch 1 --extra-hostfwd tcp::1-:2 \
    >"$WORK/out-8" 2>"$WORK/err-8"; then
    fail "vm-launch took an --extra-hostfwd without ,hostfwd="
fi
grep -q 'extra-hostfwd must be' "$WORK/err-8" ||
    fail "a malformed --extra-hostfwd wasn't refused: $(cat "$WORK/err-8")"
pass "vm-launch refuses a malformed --extra-hostfwd"

"$PYTHON" -I "$ERNICCTL" vm-stop 1 >"$WORK/out-9" 2>"$WORK/err-9" ||
    fail "ernicctl vm-stop failed: $(cat "$WORK/err-9")"
! running "$PID" || fail "vm-stop left QEMU running"
[[ -z $(vm_field vm_pid) ]] || fail "vm-stop left the VM in the manifest"
pass "vm-stop stops the guest's QEMU"

# A server that has exited, and that its parent hasn't reaped
"$PYTHON" -I -c '
import os, sys, time
pid = os.fork()
if pid == 0:
    os._exit(0)
with open(sys.argv[1] + ".tmp", "w", encoding="utf-8") as f:
    f.write(str(pid))
os.rename(sys.argv[1] + ".tmp", sys.argv[1])
time.sleep(300)
' "$WORK/zombie" &
ZOMBIE_PARENT_PID=$!
deadline=$((SECONDS + 30))
until [[ -s $WORK/zombie ]] && ! running "$(cat "$WORK/zombie")"; do
    ((SECONDS < deadline)) || fail "the stand-in server didn't become a zombie"
    sleep 0.1
done
cat >"$MANIFEST" <<EOF
{"instances": [{"id": 1, "role": "manager", "pid": $(cat "$WORK/zombie"),
  "mac": "02:00:00:00:00:01", "socket": "$SOCK", "vm": null}]}
EOF
if "$PYTHON" -I "$ERNICCTL" vm-launch 1 --boot-timeout 30 \
    >"$WORK/out-10" 2>"$WORK/err-10"; then
    fail "vm-launch started a VM for an instance whose server has exited"
fi
grep -q 'Instance 1 is not running' "$WORK/err-10" ||
    fail "a zombie server was taken for a running one: $(cat "$WORK/err-10")"
pass "ernicctl takes a zombie for an exited process"

echo "All boot-guest tests passed."
