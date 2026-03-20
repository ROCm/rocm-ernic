#!/bin/bash
#
# hot-reload.sh
#
# Rebuild and reload the rocm-ernic server without tearing
# down the running VM. Uses QEMU QMP to hot-unplug and
# hot-plug the vfio-user-pci device while the VM stays up.
#
# Prerequisites:
#   - VM launched via run-vm-vfio-user.sh (QMP + root port)
#   - socat installed on the host
#   - Guest driver (rocm_ernic.ko) already built in the guest
#
# Usage:
#   ./scripts/hot-reload.sh [OPTIONS]
#
# Options:
#   --no-build        Skip the rebuild step
#   --build-only      Only rebuild; do not cycle the device
#   --update-driver   Rebuild and reload the guest driver too
#   --backend TYPE    Backend type (default: loopback)
#   --help            Show this help message
#
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Shared conventions with local-vm-test.sh / run-vm-vfio-user.sh
SOCKET_PATH=${VFIO_USER_SOCKET:-/tmp/vfio-user-rocm-ernic.sock}
QMP_SOCKET=${QMP_SOCKET:-/tmp/qemu-qmp.sock}
SERVER_LOG="/tmp/rocm-ernic-server.log"
PID_FILE="/tmp/rocm-ernic-server.pid"
SSH_PORT=${SSH_PORT:-2222}
SSH_USER=${SSH_USER:-stebates}
BACKEND=${BACKEND:-loopback}
DEVICE_ID="ernic0"
ROOT_PORT="rp0"

# Flags
DO_BUILD=true
BUILD_ONLY=false
UPDATE_DRIVER=false

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_step()  { echo -e "${CYAN}[STEP]${NC} $1"; }

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Rebuild and reload the rocm-ernic server without"
    echo "tearing down the running VM."
    echo ""
    echo "Options:"
    echo "  --no-build        Skip the rebuild step"
    echo "  --build-only      Only rebuild; don't cycle device"
    echo "  --update-driver   Rebuild/reload guest driver too"
    echo "  --backend TYPE    Backend type (default: loopback)"
    echo "  --help            Show this help"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build)     DO_BUILD=false;    shift ;;
        --build-only)   BUILD_ONLY=true;   shift ;;
        --update-driver) UPDATE_DRIVER=true; shift ;;
        --backend)      BACKEND="$2";      shift 2 ;;
        --help)         usage ;;
        *)
            log_error "Unknown option: $1"
            usage
            ;;
    esac
done

# --- helper: send a QMP command and return the response ------
qmp_send() {
    local cmd="$1"
    printf '{"execute":"qmp_capabilities"}\n%s\n' "$cmd" \
        | socat - UNIX-CONNECT:"${QMP_SOCKET}" 2>/dev/null
}

# --- helper: wait for a process to exit (with timeout) -------
wait_for_exit() {
    local pid=$1
    local timeout=${2:-10}
    local elapsed=0
    while kill -0 "$pid" 2>/dev/null; do
        sleep 0.5
        elapsed=$((elapsed + 1))
        if [ "$elapsed" -ge $((timeout * 2)) ]; then
            log_warn "Process $pid still running after ${timeout}s"
            return 1
        fi
    done
    return 0
}

# --- helper: wait for socket to appear -----------------------
wait_for_socket() {
    local path=$1
    local timeout=${2:-15}
    local i
    for i in $(seq 1 $((timeout * 2))); do
        if [ -S "$path" ]; then
            return 0
        fi
        sleep 0.5
    done
    log_error "Socket $path not created after ${timeout}s"
    return 1
}

# --- helper: SSH into guest -----------------------------------
guest_ssh() {
    ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 \
        -p "${SSH_PORT}" "${SSH_USER}@localhost" "$@"
}

# --- preflight checks ----------------------------------------
log_info "Hot-reload: preflight checks"

if ! command -v socat &>/dev/null; then
    log_error "socat is required but not installed"
    log_error "  sudo apt install socat"
    exit 1
fi

if [ ! -S "${QMP_SOCKET}" ]; then
    log_error "QMP socket not found: ${QMP_SOCKET}"
    log_error "Was the VM started with run-vm-vfio-user.sh?"
    exit 1
fi

if [ ! -d "${PROJECT_ROOT}/build" ]; then
    log_error "Build directory not found at ${PROJECT_ROOT}/build"
    exit 1
fi

# --- step 1: rebuild -----------------------------------------
if [ "$DO_BUILD" = true ]; then
    log_step "1/6 Rebuilding rocm-ernic server..."
    cmake --build "${PROJECT_ROOT}/build"
    log_info "Build complete"
else
    log_step "1/6 Skipping build (--no-build)"
fi

if [ "$BUILD_ONLY" = true ]; then
    log_info "Build-only mode; stopping here."
    exit 0
fi

# --- step 2: unload guest driver -----------------------------
log_step "2/6 Unloading guest driver..."
if guest_ssh 'lsmod | grep -q rocm_ernic'; then
    guest_ssh 'sudo rmmod rocm_ernic'
    log_info "Guest driver unloaded"
else
    log_warn "Guest driver not loaded; skipping rmmod"
fi

# --- step 3: QMP device_del ----------------------------------
log_step "3/6 Hot-unplugging device via QMP..."
DEL_RESP=$(qmp_send \
    '{"execute":"device_del","arguments":{"id":"'"${DEVICE_ID}"'"}}')

if echo "$DEL_RESP" | grep -q '"error"'; then
    log_warn "device_del response: $DEL_RESP"
    log_warn "Device may already be removed; continuing"
fi

sleep 2
log_info "Device unplugged"

# --- step 4: stop old server ---------------------------------
log_step "4/6 Stopping old server..."
if [ -f "${PID_FILE}" ]; then
    OLD_PID=$(cat "${PID_FILE}")
    if kill -0 "$OLD_PID" 2>/dev/null; then
        sudo kill "$OLD_PID"
        if wait_for_exit "$OLD_PID" 10; then
            log_info "Old server (PID $OLD_PID) stopped"
        else
            log_warn "Force-killing server (PID $OLD_PID)"
            sudo kill -9 "$OLD_PID" 2>/dev/null || true
            sleep 1
        fi
    else
        log_warn "PID $OLD_PID not running"
    fi
    rm -f "${PID_FILE}"
else
    log_warn "No PID file found; checking for running server"
    if pgrep -f "rocm-ernic.*${SOCKET_PATH}" >/dev/null; then
        sudo pkill -f "rocm-ernic.*${SOCKET_PATH}"
        sleep 2
    fi
fi

sudo rm -f "${SOCKET_PATH}"

# --- step 5: start new server --------------------------------
log_step "5/6 Starting new server (backend: ${BACKEND})..."
sudo "${PROJECT_ROOT}/build/rocm-ernic" \
    --socket "${SOCKET_PATH}" \
    --backend "${BACKEND}" \
    --log-file "${SERVER_LOG}" &
NEW_PID=$!
echo "$NEW_PID" > "${PID_FILE}"

if ! wait_for_socket "${SOCKET_PATH}" 15; then
    log_error "New server failed to create socket"
    log_error "Check log: ${SERVER_LOG}"
    exit 1
fi

sleep 1

if ! kill -0 "$NEW_PID" 2>/dev/null; then
    log_error "New server exited unexpectedly"
    log_error "Check log: ${SERVER_LOG}"
    exit 1
fi

log_info "New server running (PID $NEW_PID)"

# --- step 6: QMP device_add ----------------------------------
log_step "6/6 Hot-plugging device via QMP..."
ADD_CMD=$(cat <<EOFQMP
{"execute":"device_add","arguments":{"driver":"vfio-user-pci","id":"${DEVICE_ID}","bus":"${ROOT_PORT}","socket":{"path":"${SOCKET_PATH}","type":"unix"}}}
EOFQMP
)
ADD_RESP=$(qmp_send "$ADD_CMD")

if echo "$ADD_RESP" | grep -q '"error"'; then
    log_error "device_add failed: $ADD_RESP"
    exit 1
fi

log_info "Device plugged in"

sleep 1

# --- optional: update guest driver ---------------------------
if [ "$UPDATE_DRIVER" = true ]; then
    log_info "Updating guest driver..."
    scp -o StrictHostKeyChecking=no \
        -P "${SSH_PORT}" -r \
        "${PROJECT_ROOT}/driver" \
        "${SSH_USER}@localhost:/tmp/"
    guest_ssh 'cd /tmp/driver && make clean && make && sudo insmod ./rocm_ernic.ko'
    log_info "Guest driver rebuilt and loaded"
else
    log_info "Loading guest driver..."
    guest_ssh 'sudo modprobe rocm_ernic' || {
        log_warn "modprobe failed; trying insmod from /tmp"
        guest_ssh 'sudo insmod /tmp/driver/rocm_ernic.ko'
    }
fi

# --- verify ---------------------------------------------------
log_info "Verifying RDMA device..."
if guest_ssh 'ibv_devices 2>/dev/null | tail -n +3 | grep -qE "rocep|mlx|qedr|rxe"'; then
    log_info "RDMA device detected in guest"
else
    log_warn "RDMA device not detected; check guest dmesg"
fi

echo ""
log_info "Hot-reload complete."
log_info "Server PID: $NEW_PID  Log: ${SERVER_LOG}"
log_info "SSH: ssh -p ${SSH_PORT} ${SSH_USER}@localhost"
