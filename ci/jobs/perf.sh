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
# The plays are thin wrappers around the roles in the in-tree
# sbates130272.rocm_ernic collection.  ansible.cfg points at them
# with a relative path; name it absolutely here so the run does
# not depend on the working directory.
export ANSIBLE_ROLES_PATH="${ANSIBLE_DIR}/roles:${HOME}/.ansible/roles"

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

# stdin is redirected from /dev/null: ansible refuses to
# start if it inherits a non-blocking stdin, which happens
# whenever this script is driven from a background shell.
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
        "$@" </dev/null
}

cd "${ANSIBLE_DIR}"

# The perf job brings up its own VMs, and vm-down.sh deleted
# the overlays the functional job left behind, so these guests
# are freshly cloned from the golden image: no driver loaded
# and no address on the emulated NIC.  Without this the very
# first check fails with "Ping failed between VMs" and every
# subsequent measurement records FAIL.
group_start "Guest setup (driver + rdma-core)"
run_check perf "guest-setup" ci_ansible --tags guest-setup
group_end

# Measuring against unprovisioned guests produces a wall of
# FAIL rows that looks like a device regression, so stop here
# instead.
require_guests_ready || die "guest provisioning failed; not measuring"

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

# run_check records failures without aborting, so that every
# check still runs.  Without this the job exited 0 even when
# every sweep failed, and only the report job noticed.
failed=$(python3 -c "
import json,sys
try:
    print(sum(1 for l in open('${CI_RESULTS}/perf.jsonl')
              if json.loads(l).get('status') == 'fail'))
except OSError:
    print(0)")
if [ "${failed}" -gt 0 ]; then
    log_error "${failed} perf check(s) failed"
    exit 1
fi
