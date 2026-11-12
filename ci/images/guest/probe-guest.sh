#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# probe-guest.sh
#
# Boots the guest being built, runs a script in it over SSH, powers it off
# and exits with the script's status. The script's output goes to stdout,
# everything else to stderr. build.sh boots the guest this way for each
# phase after the first.
#
# The guest boots without a seed, so cloud-init does nothing, and accepts
# the build's key, which the first boot's seed authorized. Without
# --persist it boots a
# throwaway overlay, so the disk is left as it was; with it, it boots the
# disk itself, and what the script changes is kept, so it's powered off
# cleanly, and failing that is an error.
#
# The disk is attached with discard=unmap, so an fstrim in the guest
# frees space in the disk image too.
#
# Usage: probe-guest.sh [--persist] [--payload DIR] [--console FILE]
#                       DISK USER KEY SCRIPT
#   DISK            the qcow2
#   USER            the account to log in as
#   KEY             the private key the guest accepts
#   SCRIPT          the script, run with bash as USER, from a copy in the
#                   guest
#   --persist       keep what the script changes
#   --payload DIR   copy DIR to /tmp/payload in the guest first
#   --console FILE  where the serial console goes (default: a file that
#                   goes when this exits)
#
# Environment:
#   GUEST_BUILD_SSH_PORT  port on 127.0.0.1 forwarded to the guest's SSH
#                         (default: 2222)
#   GUEST_BUILD_VCPUS     vCPUs (default: 4)
#   GUEST_BUILD_MEM       memory in MiB (default: 4096)
#   PROBE_BOOT_TIMEOUT    seconds to wait for SSH (default: 300)

set -euo pipefail

PROG=$(basename -- "$0")

die() {
    echo "$PROG: $*" >&2
    exit 1
}

usage() {
    echo "Usage: $PROG [--persist] [--payload DIR] [--console FILE]" \
        "DISK USER KEY SCRIPT" >&2
    exit 2
}

PERSIST=false
PAYLOAD=""
CONSOLE=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --persist)
            PERSIST=true
            shift
            ;;
        --payload)
            [[ $# -ge 2 ]] || usage
            PAYLOAD=$2
            shift 2
            ;;
        --console)
            [[ $# -ge 2 ]] || usage
            CONSOLE=$2
            shift 2
            ;;
        -h | --help) usage ;;
        -*) die "unknown option: $1" ;;
        *) break ;;
    esac
done
[[ $# -eq 4 ]] || usage
DISK=$1
GUEST_USER=$2
KEY=$3
SCRIPT=$4

PORT=${GUEST_BUILD_SSH_PORT:-2222}
VCPUS=${GUEST_BUILD_VCPUS:-4}
MEM=${GUEST_BUILD_MEM:-4096}
BOOT_TIMEOUT=${PROBE_BOOT_TIMEOUT:-300}
for value in "$PORT" "$VCPUS" "$MEM" "$BOOT_TIMEOUT"; do
    [[ $value =~ ^[1-9][0-9]{0,5}$ ]] || die "not a number: '$value'"
done

[[ -f $DISK ]] || die "no disk at $DISK"
[[ -f $KEY ]] || die "no key at $KEY"
[[ -f $SCRIPT ]] || die "no script at $SCRIPT"
[[ -z $PAYLOAD || -d $PAYLOAD ]] || die "no directory at $PAYLOAD"
# QEMU reads a comma as the end of an option's value
[[ $DISK != *,* ]] || die "the disk's path can't contain a comma: $DISK"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/probe-guest-XXXXXX")
CONSOLE=${CONSOLE:-$TMP/console.log}
[[ $CONSOLE != *,* ]] || die "the console's path can't contain a comma"

QEMU_PID=""
# shellcheck disable=SC2329 # run by the EXIT trap
cleanup() {
    if [[ -n $QEMU_PID ]]; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf -- "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if $PERSIST; then
    BOOT_DISK=$DISK
    echo "$PROG: booting $DISK; changes will be kept" >&2
else
    BOOT_DISK=$TMP/overlay.qcow2
    qemu-img create -q -f qcow2 -F qcow2 -b "$(realpath -- "$DISK")" \
        "$BOOT_DISK"
    echo "$PROG: booting an overlay on $DISK" >&2
fi

qemu-system-x86_64 \
    -machine q35,accel=kvm \
    -cpu host \
    -smp "$VCPUS" \
    -m "$MEM" \
    -drive "if=virtio,format=qcow2,file=$BOOT_DISK,discard=unmap,detect-zeroes=unmap" \
    -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$PORT-:22" \
    -device virtio-net-pci,netdev=n0 \
    -display none \
    -monitor none \
    -serial "file:$CONSOLE" \
    </dev/null >"$TMP/qemu.log" 2>&1 &
QEMU_PID=$!

SSH_OPTS=(
    -F /dev/null -i "$KEY" -o IdentitiesOnly=yes -o BatchMode=yes
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
    -o LogLevel=ERROR -o ConnectTimeout=5
)
SSH=(ssh "${SSH_OPTS[@]}" -p "$PORT" "$GUEST_USER@127.0.0.1")

deadline=$((SECONDS + BOOT_TIMEOUT))
until timeout 30 "${SSH[@]}" true </dev/null 2>/dev/null; do
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        QEMU_PID=""
        echo "$PROG: QEMU exited:" >&2
        cat -- "$TMP/qemu.log" >&2
        exit 1
    fi
    if ((SECONDS >= deadline)); then
        echo "$PROG: the guest didn't accept SSH within ${BOOT_TIMEOUT}s." \
            "Its console ends:" >&2
        tail -n 100 -- "$CONSOLE" >&2 || true
        exit 1
    fi
    sleep 5
done
echo "$PROG: SSH is up" >&2

if [[ -n $PAYLOAD ]]; then
    echo "$PROG: copying $PAYLOAD to /tmp/payload in the guest" >&2
    "${SSH[@]}" 'rm -rf /tmp/payload' </dev/null
    scp "${SSH_OPTS[@]}" -P "$PORT" -r -- "$PAYLOAD" \
        "$GUEST_USER@127.0.0.1:/tmp/payload"
fi

# Copied in and run from there, not fed to `bash -s`, where a command
# reading its input would read the rest of the script
"${SSH[@]}" 'cat >/tmp/probe-guest.sh' <"$SCRIPT"
status=0
# shellcheck disable=SC2016 # expanded in the guest
"${SSH[@]}" 'bash /tmp/probe-guest.sh; rc=$?; rm -f /tmp/probe-guest.sh;
    exit "$rc"' </dev/null || status=$?
if [[ $status -ne 0 ]]; then
    echo "$PROG: the script failed with status $status" >&2
fi

# The connection drops as the guest goes down, so ssh's status says
# nothing; QEMU exiting does. A script that removed the key powers the
# guest off itself.
"${SSH[@]}" 'sudo -n systemctl poweroff' </dev/null >/dev/null 2>&1 || true
for _ in {1..60}; do
    kill -0 "$QEMU_PID" 2>/dev/null || break
    sleep 2
done
if kill -0 "$QEMU_PID" 2>/dev/null; then
    echo "$PROG: the guest didn't power off within 120s" >&2
    if $PERSIST; then
        # Killing it could leave the disk's filesystem unclean
        exit 1
    fi
else
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=""
fi
exit "$status"
