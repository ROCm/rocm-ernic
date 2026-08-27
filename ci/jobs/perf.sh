#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# ci/jobs/perf.sh -- RDMA and TCP/IP performance sweeps
# across the two CI VMs.
#
# Drives the existing performance plays, which write
# timestamped CSVs (bw-*.csv, lat-*.csv,
# reliability-*.csv).  Those CSVs are the input to
# ci/report/gen-report.py.
#
# Emits:
#   $CI_RESULTS/perf.jsonl
#   $CI_RESULTS/perf-csv/*.csv

# shellcheck source=/dev/null
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

ernic_env
mkdir -p "${CI_RESULTS}/perf-csv" "${CI_RESULTS}/junit"

# Refuse to produce numbers that would be recorded as
# a performance baseline but measured under software
# emulation.  A TCG run is 1-2 orders of magnitude off
# and would poison the trend history.
if [ "${CI_VM_ACCEL:-kvm}" != "kvm" ]; then
    die "perf requires KVM (CI_VM_ACCEL=${CI_VM_ACCEL:-kvm}); refusing to record emulated numbers"
fi
require_kvm || die "perf requires an accessible /dev/kvm"

ANSIBLE_DIR="${PROJECT_ROOT}/ansible"
export ANSIBLE_CALLBACKS_ENABLED=junit
export JUNIT_OUTPUT_DIR="${CI_RESULTS}/junit"
export JUNIT_HIDE_TASK_ARGUMENTS=yes
export ANSIBLE_HOST_KEY_CHECKING=false

# Point the perf plays at the CI results dir rather
# than docs/perf-results, so a CI run never dirties the
# working tree it just checked out.
PERF_CSV_DIR="${CI_RESULTS}/perf-csv"

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
    ansible-playbook ci-site.yml \
        -e "ernic_project_root=${PROJECT_ROOT}" \
        -e "ernic_run_dir=${CI_RUN_DIR}" \
        -e "ernic_log_dir=${CI_LOG_DIR}" \
        -e "ernic_instances=${ERNIC_INSTANCES}" \
        -e "ernic_vm_ssh_base_port=${CI_VM_SSH_BASE_PORT}" \
        -e "ernic_vm_ssh_user=${CI_VM_SSH_USER}" \
        -e "results_dir=${PERF_CSV_DIR}" \
        -e "ernic_golden_image=false" \
        -e "ernic_build=false" \
        -e "ernic_gpu_passthrough=${CI_GPU_PASSTHROUGH}" \
        "$@"
}

cd "${ANSIBLE_DIR}"

group_start "TCP/IP throughput (iperf3)"
run_check perf "tcp-perf" ci_ansible --tags tcp-perf
group_end

# The RDMA sweep is the long pole (tens of minutes). If a
# server instance is already gone there is nothing left to
# measure, so stop instead of recording a wall of FAILs.
require_instances_alive || \
    die "rocm-ernic instance died before the RDMA sweep"

group_start "RDMA bandwidth / latency sweeps (perftest)"
run_check perf "rdma-perf" ci_ansible --tags rdma-perf
group_end

run_check perf "instances-survived" require_instances_alive

group_start "Collect CSVs"
csv_count=$(find "${PERF_CSV_DIR}" -name '*.csv' | wc -l)
log_info "collected ${csv_count} CSV file(s) in ${PERF_CSV_DIR}"
run_check perf "csv-produced" test "${csv_count}" -gt 0
group_end

log_info "perf job complete; results in ${CI_RESULTS}/perf.jsonl"
