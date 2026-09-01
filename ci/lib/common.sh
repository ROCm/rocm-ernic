# shellcheck shell=bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# common.sh -- shared helpers for the rocm-ernic
# self-hosted CI jobs.
#
# Source this from any ci/jobs/*.sh script:
#
#   . "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"
#
# Everything here runs unprivileged.  The CI never uses
# sudo, systemd, /run or /var/log: the rocm-ernic
# launcher and ernicctl are fully env-driven, so the
# whole control plane is redirected under CI_WORK.

set -euo pipefail

CI_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CI_ROOT="$(cd "${CI_LIB_DIR}/.." && pwd)"
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "${CI_ROOT}/.." && pwd)}"

# ── Workspace layout ──────────────────────────────
#
# CI_WORK lives on local disk: $HOME is NFS on this
# node and qcow2 / build I/O there is slow.

CI_WORK="${CI_WORK:-/var/tmp/ernic-ci-work}"
CI_BUILD_DIR="${CI_BUILD_DIR:-${CI_WORK}/build}"
CI_RESULTS="${CI_RESULTS:-${CI_WORK}/results}"
CI_RUN_DIR="${CI_RUN_DIR:-${CI_WORK}/run}"
CI_LOG_DIR="${CI_LOG_DIR:-${CI_WORK}/log}"

# ── rocm-ernic service settings (user-scoped) ─────

ERNIC_INSTANCES="${ERNIC_INSTANCES:-2}"
ERNIC_TCP_PORT="${ERNIC_TCP_PORT:-6420}"
# Prefer the checked-out copies over whatever is
# installed system-wide: CI must test the tree it was
# handed, and /usr/local is root-owned so a CI run can
# never refresh it.
ERNIC_LAUNCHER="${ERNIC_LAUNCHER:-${PROJECT_ROOT}/service/rocm-ernic-launcher}"
ERNICCTL="${ERNICCTL:-${PROJECT_ROOT}/service/ernicctl}"

# ── VM settings ───────────────────────────────────
#
# Deliberately distinct from the interactive defaults
# in /etc/rocm-ernic/rocm-ernic.env (vm name base and
# ssh base port 2250) so CI never disturbs or reuses a
# developer's VMs and overlays on this box.

CI_VM_NAME_BASE="${CI_VM_NAME_BASE:-rocm-ernic-ci-vm}"
CI_VM_SSH_BASE_PORT="${CI_VM_SSH_BASE_PORT:-2350}"
CI_VM_SSH_USER="${CI_VM_SSH_USER:-ubuntu}"
CI_VM_VCPUS="${CI_VM_VCPUS:-8}"
CI_VM_MEM="${CI_VM_MEM:-16384}"
CI_VM_IMAGE_DIR="${CI_VM_IMAGE_DIR:-/opt/qemu-images}"
CI_VM_BACKING="${CI_VM_BACKING:-/opt/qemu-images/backing/rocm-ernic-may-27-vm-backing.qcow2}"
CI_QEMU_MINIMAL="${CI_QEMU_MINIMAL:-${HOME}/Projects/qemu-minimal}"
CI_QEMU_PATH="${CI_QEMU_PATH:-/opt/qemu-10.2.2-pci-mmio-bridge-submit/bin/}"

# ansible/group_vars/all.yml defaults GPU passthrough on,
# but vm-up.sh launches without --pci-hostdev, so the
# guest-setup play would try to build rocm-xio against a
# GPU that was never handed to the guest.  Keep the
# playbook's view aligned with how CI actually launches.
# Set true only alongside CI_VM_PCI_HOSTDEV.
CI_GPU_PASSTHROUGH="${CI_GPU_PASSTHROUGH:-false}"

# ── Logging ───────────────────────────────────────

_ts() { date -u '+%H:%M:%S'; }

log_info()  { echo "[$(_ts)] INFO  $*"; }
log_warn()  { echo "[$(_ts)] WARN  $*" >&2; }
log_error() { echo "[$(_ts)] ERROR $*" >&2; }

# Emit a GitHub Actions log group when running under
# Actions; a plain header otherwise.
group_start() {
    if [ -n "${GITHUB_ACTIONS:-}" ]; then
        echo "::group::$*"
    else
        echo "── $* ──"
    fi
}

group_end() {
    if [ -n "${GITHUB_ACTIONS:-}" ]; then
        echo "::endgroup::"
    fi
}

die() { log_error "$*"; exit 1; }

# ── Result recording ──────────────────────────────
#
# Every check appends one JSON object to
# $CI_RESULTS/<suite>.jsonl.  ci/report/gen-report.py
# turns those into the functional report and the
# GitHub step summary.  Keeping this append-only means
# a job that dies mid-way still reports what it did.

record_result() {
    # record_result <suite> <name> <status> <duration_s> [detail]
    local suite="$1" name="$2" status="$3" dur="$4"
    local detail="${5:-}"
    mkdir -p "${CI_RESULTS}"
    python3 - "$suite" "$name" "$status" "$dur" "$detail" \
        >>"${CI_RESULTS}/${suite}.jsonl" <<'PY'
import json, sys, time
suite, name, status, dur, detail = sys.argv[1:6]
json.dump({
    "suite": suite,
    "name": name,
    "status": status,
    "duration_s": float(dur or 0),
    "detail": detail,
    "ts": time.time(),
}, sys.stdout)
sys.stdout.write("\n")
PY
}

# Run a named check, time it, and record pass/fail.
# Never aborts the job: the report is the source of
# truth and we want every check attempted.
run_check() {
    # run_check <suite> <name> <command...>
    local suite="$1" name="$2"; shift 2
    local start end rc out
    start=$(date +%s.%N)
    set +e
    out="$("$@" 2>&1)"
    rc=$?
    set -e
    end=$(date +%s.%N)
    local dur
    dur=$(python3 -c "print(f'{${end}-${start}:.3f}')")

    if [ "$rc" -eq 0 ]; then
        log_info "PASS  ${name} (${dur}s)"
        record_result "$suite" "$name" pass "$dur" ""
    else
        log_error "FAIL  ${name} (${dur}s, rc=${rc})"
        echo "$out" | tail -20
        record_result "$suite" "$name" fail "$dur" \
            "$(echo "$out" | tail -5)"
    fi
    return 0
}

# ── Environment for ernicctl / launcher ───────────
#
# Exports the full user-scoped config.  Call before
# any ernicctl or launcher invocation.

ernic_env() {
    export ERNIC_BIN="${CI_BUILD_DIR}/rocm-ernic"
    export ERNIC_RUN_DIR="${CI_RUN_DIR}"
    export ERNIC_LOG_DIR="${CI_LOG_DIR}"
    export ERNIC_INSTANCES
    export ERNIC_TCP_PORT
    export ERNIC_VM_IMAGE_DIR="${CI_VM_IMAGE_DIR}"
    export ERNIC_VM_BACKING="${CI_VM_BACKING}"
    export ERNIC_VM_NAME="${CI_VM_NAME_BASE}"
    export ERNIC_VM_SSH_BASE_PORT="${CI_VM_SSH_BASE_PORT}"
    export ERNIC_VM_SSH_USER="${CI_VM_SSH_USER}"
    export ERNIC_QEMU_MINIMAL="${CI_QEMU_MINIMAL}"
    export ERNIC_QEMU_PATH="${CI_QEMU_PATH}"
    [ -n "${CI_VM_SSH_IDENTITY:-}" ] && \
        export ERNIC_VM_SSH_IDENTITY="${CI_VM_SSH_IDENTITY}"
    return 0
}

# ssh command for CI VM N (1-based).
vm_ssh_port() { echo $(( CI_VM_SSH_BASE_PORT + $1 - 1 )); }

vm_ssh() {
    local n="$1"; shift
    ssh -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o PasswordAuthentication=no \
        -o ConnectTimeout=10 \
        -o LogLevel=ERROR \
        -p "$(vm_ssh_port "$n")" \
        "${CI_VM_SSH_USER}@localhost" "$@"
}

# ── Preflight ─────────────────────────────────────

# Verify every instance in the manifest is still alive.
#
# A rocm-ernic server that dies mid-run takes every
# subsequent test down with it, but each one fails slowly
# and on its own terms.  Observed in practice: both
# instances died five minutes into a sweep and the job
# spent another 51 minutes recording FAIL rows that all
# shared one root cause.  Checking between stages turns
# that into a fast, correctly attributed failure.
require_instances_alive() {
    local manifest="${CI_RUN_DIR}/instances.json"
    if [ ! -f "${manifest}" ]; then
        log_error "no instance manifest at ${manifest}"
        return 1
    fi

    local dead
    dead="$(python3 - "${manifest}" <<'PY'
import json, os, sys
dead = []
for inst in json.load(open(sys.argv[1]))["instances"]:
    pid = inst.get("pid") or 0
    try:
        os.kill(pid, 0)
    except (OSError, ProcessLookupError):
        dead.append(f"{inst['id']}(pid={pid})")
print(",".join(dead))
PY
)"

    if [ -n "${dead}" ]; then
        log_error "rocm-ernic instance(s) died: ${dead}"
        log_error "tail of each instance log:"
        for f in "${CI_LOG_DIR}"/[0-9].log; do
            [ -f "${f}" ] || continue
            log_error "  --- ${f} ---"
            tail -5 "${f}" >&2 || true
        done
        return 1
    fi
    return 0
}

# Verify each guest is actually provisioned: RDMA device
# present, port active, and an address on the emulated NIC.
#
# The perf job clones fresh guests from the golden image, so
# skipping guest-setup leaves them with no driver and no
# address.  Every measurement then records FAIL, which reads
# like a device regression rather than a setup mistake.
require_guests_ready() {
    local i ok=0
    for i in $(seq 1 "${ERNIC_INSTANCES}"); do
        if ! vm_ssh "${i}" 'ibv_devices' 2>/dev/null \
                | grep -qE 'rocm-rdma-ernic|rocep'; then
            log_error "guest ${i}: no RDMA device"
            ok=1
            continue
        fi
        if ! vm_ssh "${i}" 'ibv_devinfo' 2>/dev/null \
                | grep -q 'PORT_ACTIVE'; then
            log_error "guest ${i}: port not active"
            ok=1
            continue
        fi
        if ! vm_ssh "${i}" \
                'ip -4 -br addr show rocm-ernic0' 2>/dev/null \
                | grep -qE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+'; then
            log_error "guest ${i}: no address on rocm-ernic0"
            ok=1
        fi
    done
    return "${ok}"
}

require_kvm() {
    if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
        log_error "/dev/kvm is not accessible to $(id -un)."
        log_error "  $(ls -l /dev/kvm)"
        log_error "  kvm group is $(getent group kvm | cut -d: -f3), you are in: $(id -Gn | tr ' ' ',')"
        log_error "Fix with:  sudo chgrp kvm /dev/kvm && sudo chmod 660 /dev/kvm"
        return 1
    fi
    return 0
}
