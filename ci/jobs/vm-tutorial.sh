#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# ci/jobs/vm-tutorial.sh -- 2-VM RDMA-Tutorial lane over the
# emulated NICs.
#
# Provisions the guests (kernel driver + rdma-core provider +
# NIC addressing) and runs the RDMA-Tutorial revisions selected
# by ansible/group_vars/all.yml through ansible/ci-site.yml.
#
# Emits:
#   $CI_RESULTS/vm-tutorial.jsonl
#   $CI_RESULTS/junit/*.xml

# shellcheck source=/dev/null
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

ernic_env
mkdir -p "${CI_RESULTS}/junit"
start_suite vm-tutorial

ANSIBLE_DIR="${PROJECT_ROOT}/ansible"
[ -f "${ANSIBLE_DIR}/ci-site.yml" ] || \
    die "ci-site.yml not found under ${ANSIBLE_DIR}"

export ANSIBLE_CALLBACKS_ENABLED=junit
export JUNIT_OUTPUT_DIR="${CI_RESULTS}/junit"
export JUNIT_FAIL_ON_IGNORE=yes
export JUNIT_HIDE_TASK_ARGUMENTS=yes
export ANSIBLE_HOST_KEY_CHECKING=false
export ANSIBLE_ROLES_PATH="${ANSIBLE_DIR}/roles:${HOME}/.ansible/roles"

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
        -e "ernic_build=false" \
        -e "ernic_gpu_passthrough=${CI_GPU_PASSTHROUGH}" \
        "$@" </dev/null
}

cd "${ANSIBLE_DIR}" || die "cannot cd to ${ANSIBLE_DIR}"

group_start "Guest setup (driver + rdma-core)"
run_check vm-tutorial "guest-setup" \
    ci_ansible --tags guest-setup
group_end

group_start "Guest reachability"
run_check vm-tutorial "guests-ready" require_guests_ready
group_end

group_start "RDMA tutorial examples"
run_check vm-tutorial "tutorial-tests" \
    ci_ansible --tags tutorial
group_end

group_start "Instance liveness"
run_check vm-tutorial "instances-survived" require_instances_alive
group_end

log_info "vm-tutorial job complete; results in ${CI_RESULTS}/vm-tutorial.jsonl"
