#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# ci/jobs/vm-functional.sh -- 2-VM RDMA functional
# tests over the emulated NICs.
#
# Provisions the guests (kernel driver + rdma-core v62
# provider + NIC addressing) and runs the sanity suite:
# ping, iperf3, ibv_rc_pingpong and a perftest
# handshake between the two VMs.
#
# The test logic lives in the existing Ansible plays;
# this wrapper supplies CI-specific variables and turns
# Ansible task results into the CI's JSON result stream
# via the junit callback.
#
# Emits:
#   $CI_RESULTS/vm-functional.jsonl
#   $CI_RESULTS/junit/*.xml

# shellcheck source=/dev/null
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

ernic_env
mkdir -p "${CI_RESULTS}/junit"

ANSIBLE_DIR="${PROJECT_ROOT}/ansible"
[ -f "${ANSIBLE_DIR}/ci-site.yml" ] || \
    die "ci-site.yml not found under ${ANSIBLE_DIR}"

# The junit callback writes one XML file per play,
# which the report generator merges with the shell-level
# results.  ANSIBLE_CALLBACKS_ENABLED is the 2.16 spelling.
export ANSIBLE_CALLBACKS_ENABLED=junit
export JUNIT_OUTPUT_DIR="${CI_RESULTS}/junit"
export JUNIT_FAIL_ON_IGNORE=yes
# Task names otherwise carry a full argument dump, which
# makes the report table unreadable.
export JUNIT_HIDE_TASK_ARGUMENTS=yes
export ANSIBLE_HOST_KEY_CHECKING=false

# The junit callback has no v2_runner_on_unreachable
# hook, so unreachable hosts produce no testcase at all
# and a run where every guest is down would otherwise
# render as all-green.  ansible-playbook still exits 4,
# which run_check records, but assert it explicitly so
# the cause is named rather than inferred.
ci_ansible() {
    local rc=0 out
    out="$(_ci_ansible_run "$@" 2>&1)" || rc=$?
    echo "${out}"
    if echo "${out}" | grep -qE 'unreachable=[1-9]'; then
        log_error "one or more guests were unreachable"
        return 4
    fi
    return "${rc}"
}

_ci_ansible_run() {
    # shellcheck disable=SC2086
    ansible-playbook ci-site.yml \
        -e "ernic_project_root=${PROJECT_ROOT}" \
        -e "ernic_build_dir=${CI_BUILD_DIR}" \
        -e "ernic_run_dir=${CI_RUN_DIR}" \
        -e "ernic_log_dir=${CI_LOG_DIR}" \
        -e "ernic_instances=${ERNIC_INSTANCES}" \
        -e "ernic_vm_ssh_base_port=${CI_VM_SSH_BASE_PORT}" \
        -e "ernic_vm_ssh_user=${CI_VM_SSH_USER}" \
        -e "ernic_vm_name_base=${CI_VM_NAME_BASE}" \
        -e "ernic_golden_image=false" \
        -e "ernic_build=false" \
        -e "ernic_gpu_passthrough=${CI_GPU_PASSTHROUGH}" \
        "$@"
}

cd "${ANSIBLE_DIR}"

group_start "Guest setup (driver + rdma-core)"
run_check vm-functional "guest-setup" \
    ci_ansible --tags guest-setup
group_end

group_start "RDMA sanity suite"
run_check vm-functional "sanity-tests" \
    ci_ansible --tags sanity
group_end

# ── Direct connectivity probes ────────────────────
#
# The Ansible sanity play uses failed_when: false in
# places so it can report everything before asserting.
# These probes are the hard gate: if the RDMA devices
# are not present, active and able to move bytes between
# the two guests, the functional run has proven nothing.

probe_rdma_device() {
    vm_ssh "$1" 'ibv_devices' \
        | grep -qE 'rocm-rdma-ernic|rocep'
}

probe_port_active() {
    vm_ssh "$1" 'ibv_devinfo' | grep -q 'PORT_ACTIVE'
}

# Device name and NIC address are discovered rather than
# assumed: the guest names the device from its GUID and
# the play assigns .10/.20 from ernic_nic_subnet.
guest_rdma_dev() {
    # Single-quoted on purpose: the awk body must reach
    # the guest shell unexpanded.
    # shellcheck disable=SC2016
    vm_ssh "$1" \
        'ibv_devices | awk "NR>2 {print \$1; exit}"' \
        | tr -d '\r'
}

guest_rdma_ip() {
    vm_ssh "$1" \
        "ip -4 -br addr show rocm-ernic0 \
         | awk '{print \$3}' | cut -d/ -f1" | tr -d '\r'
}

# End-to-end RC data transfer between the two guests.
# This is the check that actually proves the emulated
# NIC carries RDMA traffic.
probe_pingpong() {
    local dev1 dev2 ip1
    dev1="$(guest_rdma_dev 1)"
    dev2="$(guest_rdma_dev 2)"
    ip1="$(guest_rdma_ip 1)"

    if [ -z "${dev1}" ] || [ -z "${dev2}" ] || [ -z "${ip1}" ]; then
        log_error "could not discover RDMA device/address" \
                  "(dev1=${dev1} dev2=${dev2} ip1=${ip1})"
        return 1
    fi

    vm_ssh 1 "nohup ibv_rc_pingpong -d ${dev1} -g 1 -n 50 \
        >/tmp/ci-pingpong.log 2>&1 & echo ok" >/dev/null
    sleep 3

    local out
    out="$(vm_ssh 2 "ibv_rc_pingpong -d ${dev2} -g 1 -n 50 ${ip1}" 2>&1)"
    echo "${out}"
    # Both the byte count and the iteration line must be
    # present; a connected-but-idle QP prints neither.
    echo "${out}" | grep -q 'bytes in' && \
        echo "${out}" | grep -q 'iters in'
}

group_start "RDMA device probes"
for i in $(seq 1 "${ERNIC_INSTANCES}"); do
    run_check vm-functional "vm-${i}-rdma-device" \
        probe_rdma_device "${i}"
    run_check vm-functional "vm-${i}-port-active" \
        probe_port_active "${i}"
done
group_end

group_start "RDMA data transfer"
run_check vm-functional "rc-pingpong" probe_pingpong
group_end

# A server that crashed during the run invalidates every
# result above it, so surface it as its own check rather
# than leaving it to be inferred from guest-side noise.
group_start "Instance liveness"
run_check vm-functional "instances-survived" require_instances_alive
group_end

log_info "vm-functional job complete; results in ${CI_RESULTS}/vm-functional.jsonl"
