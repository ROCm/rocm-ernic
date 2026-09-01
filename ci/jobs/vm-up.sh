#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# ci/jobs/vm-up.sh -- bring up the rocm-ernic instances
# and the CI VMs attached to them.
#
# This replaces the root-requiring parts of
# ansible/playbooks/{host-setup,vm-create}.yml.  Those
# plays install to /usr/local, drive systemd and build
# the golden image over qemu-nbd; none of that is
# available to (or wanted from) an unprivileged CI run.
# The golden backing image is treated as a prebuilt
# input instead -- see ci/README.md.
#
# Emits: $CI_RESULTS/vm-up.jsonl

# shellcheck source=/dev/null
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

ernic_env
mkdir -p "${CI_RESULTS}" "${CI_RUN_DIR}" "${CI_LOG_DIR}"

[ -x "${CI_BUILD_DIR}/rocm-ernic" ] || \
    die "no build at ${CI_BUILD_DIR}; run ci/jobs/build.sh first"
[ -f "${CI_VM_BACKING}" ] || \
    die "golden backing image not found: ${CI_VM_BACKING}"

# CI_VM_ACCEL=tcg drops to software emulation so the
# functional suite can still run without /dev/kvm.
# Perf numbers from a TCG run are meaningless and
# ci/jobs/perf.sh refuses to run under it.
CI_VM_ACCEL="${CI_VM_ACCEL:-kvm}"
if [ "${CI_VM_ACCEL}" = "kvm" ]; then
    require_kvm || die "KVM required; set CI_VM_ACCEL=tcg to emulate"
    export KVM=enable
else
    log_warn "running VMs under TCG emulation -- functional only, no valid perf"
    export KVM=disable
fi

group_start "Start rocm-ernic instances"
"${ERNIC_LAUNCHER}" --stop >/dev/null 2>&1 || true
run_check vm-up "launcher-start" "${ERNIC_LAUNCHER}"
group_end

group_start "Launch CI VMs"
for i in $(seq 1 "${ERNIC_INSTANCES}"); do
    run_check vm-up "vm-${i}-launch" \
        "${ERNICCTL}" vm-launch "${i}" \
            --vm-name "${CI_VM_NAME_BASE}-${i}" \
            --ssh-port "$(vm_ssh_port "${i}")" \
            --ssh-user "${CI_VM_SSH_USER}" \
            --vcpus "${CI_VM_VCPUS}" \
            --mem "${CI_VM_MEM}"
done
group_end

# ── Wait for guest SSH ────────────────────────────
#
# TCG boots are an order of magnitude slower than KVM,
# so the budget scales with the accelerator.

if [ "${CI_VM_ACCEL}" = "kvm" ]; then
    boot_timeout="${CI_VM_BOOT_TIMEOUT:-300}"
else
    boot_timeout="${CI_VM_BOOT_TIMEOUT:-1800}"
fi

wait_for_ssh() {
    local n="$1" deadline
    deadline=$(( $(date +%s) + boot_timeout ))
    while [ "$(date +%s)" -lt "${deadline}" ]; do
        if vm_ssh "${n}" true 2>/dev/null; then
            return 0
        fi
        # A dead qemu will never come back; fail fast
        # instead of burning the whole timeout.
        if ! pgrep -f "qemu-system.*${CI_VM_NAME_BASE}-${n}" >/dev/null; then
            log_error "qemu for VM ${n} exited; last log lines:"
            tail -20 "${CI_LOG_DIR}/vm-${n}.log" >&2 || true
            return 1
        fi
        sleep 10
    done
    log_error "VM ${n} did not reach SSH within ${boot_timeout}s"
    tail -20 "${CI_LOG_DIR}/vm-${n}.log" >&2 || true
    return 1
}

group_start "Wait for guest SSH (timeout ${boot_timeout}s)"
for i in $(seq 1 "${ERNIC_INSTANCES}"); do
    run_check vm-up "vm-${i}-ssh" wait_for_ssh "${i}"
done
group_end

log_info "vm-up job complete; results in ${CI_RESULTS}/vm-up.jsonl"
