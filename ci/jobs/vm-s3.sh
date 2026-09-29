#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# ci/jobs/vm-s3.sh -- single-VM S3-over-RDMA functional
# test against the in-process object store.
#
# Like vm-nvmeof.sh and unlike vm-functional.sh this needs
# no peer: each rocm-ernic instance terminates the HTTP
# control plane on the emulated NIC and serves object bytes
# straight out of the guest's own registered memory, so one
# guest and one server are a complete object fabric.  Bring
# the lane up with:
#
#   ERNIC_INSTANCES=1 ERNIC_BACKEND=s3:size=256M \
#       bash ci/jobs/vm-up.sh
#   bash ci/jobs/vm-s3.sh
#
# The guest side is tests/s3_rdma_client.c, compiled in the
# guest the way tests/rdma_verify.c is: it mints an
# x-amz-rdma-token over a registered buffer, PUTs and GETs
# through it, and compares the bytes that actually landed.
#
# ansible/playbooks/s3-tests.yml checks the same things and
# is what the hosted system-test-s3 job runs.  This stays
# shell because each check has to land in the CI report as
# its own JSONL record, the same split the repo already has
# between sanity-tests.yml and vm-functional.sh.  Keep the
# two in step.
#
# Emits: $CI_RESULTS/vm-s3.jsonl

# shellcheck source=/dev/null
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

ernic_env
mkdir -p "${CI_RESULTS}"
start_suite vm-s3

# The endpoint's address, port and bucket have to match what
# the instances were started with; all three default to the
# server's own defaults, which sit on the same subnet the
# guest NIC is addressed from (ernic_nic_subnet in
# group_vars).
S3_ADDR="${S3_ADDR:-192.168.200.1}"
S3_PORT="${S3_PORT:-9000}"
S3_BUCKET="${S3_BUCKET:-ernic}"
# The buffer the client registers.  Half of it is the PUT
# source and half the GET destination, so it has to be at
# least twice the largest size in the sweep.
S3_BUFFER="${S3_BUFFER:-8388608}"
S3_PERF_ITERS="${S3_PERF_ITERS:-20}"

case "${ERNIC_BACKEND:-}" in
s3*) ;;
*)
    log_warn "ERNIC_BACKEND is '${ERNIC_BACKEND:-<unset>}', not s3;"
    log_warn "the instances are not running an object store to talk to"
    ;;
esac

# ── Guest provisioning ────────────────────────────
#
# vm-down.sh deletes the qcow2 overlay at the end of every
# job, so the VM vm-up.sh just booted is a pristine clone of
# the backing image: no ionic_rdma built for the running
# kernel and no address on rocm-ernic0 -- and without that
# address the driver populates no RoCEv2 GID, so the client
# has no GID to mint into its token and, more immediately,
# no route to the endpoint.

ANSIBLE_DIR="${PROJECT_ROOT}/ansible"

export JUNIT_HIDE_TASK_ARGUMENTS=yes
export ANSIBLE_HOST_KEY_CHECKING=false
export ANSIBLE_ROLES_PATH="${ANSIBLE_DIR}/roles:${HOME}/.ansible/roles"

# ansible-playbook exits 4 on unreachable hosts but the junit
# callback emits no testcase for them, so a run where the
# guest never came up would otherwise render all-green.
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

[ -f "${ANSIBLE_DIR}/ci-site.yml" ] || \
    die "ci-site.yml not found under ${ANSIBLE_DIR}"

cd "${ANSIBLE_DIR}" || die "cannot cd to ${ANSIBLE_DIR}"

group_start "Guest setup (driver + rdma-core + NIC address)"
run_check vm-s3 "guest-setup" ci_ansible --tags guest-setup
run_check vm-s3 "guests-ready" require_guests_ready
group_end

cd "${PROJECT_ROOT}" || die "cannot cd to ${PROJECT_ROOT}"

# ── Guest-side helpers ────────────────────────────

GUEST_DIR=/tmp/s3-rdma

guest_nic() {
    vm_ssh "$1" 'ip -4 -br addr show rocm-ernic0' 2>/dev/null \
        | grep -qE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+'
}

# The endpoint is a userspace TCP/IP stack inside the
# emulator, reached over the emulated NIC rather than over
# any host route.  ICMP echo is the cheapest proof that the
# ARP exchange and the IPv4 layer under the object protocol
# both work, and it fails in a far more legible way than a
# socket timeout three checks later.
probe_endpoint_up() {
    local out
    out="$(vm_ssh "$1" "ping -c 3 -W 2 -I rocm-ernic0 ${S3_ADDR}" 2>&1)"
    echo "${out}"
    grep -qE '[123] received' <<<"${out}"
}

# The bucket listing over plain HTTP, no RDMA involved: the
# control plane on its own.  An ordinary S3 client that has
# never heard of a token sees exactly this.
probe_http_control() {
    local out
    out="$(vm_ssh "$1" "curl -sS --max-time 20 \
        http://${S3_ADDR}:${S3_PORT}/${S3_BUCKET}/" 2>&1)"
    echo "${out}"
    grep -q 'ListBucketResult' <<<"${out}"
}

# The client is compiled in the guest rather than shipped as
# a binary: the guest kernel and rdma-core are the ones the
# image pins, and a host-built binary would link against
# whatever the runner happens to have.
build_client() {
    local n="$1"

    vm_ssh "${n}" "mkdir -p ${GUEST_DIR}" >/dev/null || return 1
    vm_ssh "${n}" "cat > ${GUEST_DIR}/s3_rdma_client.c" \
        <"${PROJECT_ROOT}/tests/s3_rdma_client.c" || return 1
    vm_ssh "${n}" "cd ${GUEST_DIR} && cc -O2 -Wall -Wextra \
        -o s3_rdma_client s3_rdma_client.c \
        \$(pkg-config --libs libibverbs)" 2>&1
}

# PUT, GET, ranged GET, short-buffer GET, body GET, list,
# delete and the error cases -- and, in every transfer case,
# a byte-for-byte comparison.  A store that answers 200
# without moving bytes fails here and nowhere else.
probe_functional() {
    local n="$1" out
    out="$(vm_ssh "${n}" "cd ${GUEST_DIR} && timeout 180 \
        ./s3_rdma_client -a ${S3_ADDR} -p ${S3_PORT} -b ${S3_BUCKET} \
        -s ${S3_BUFFER} -F" 2>&1)"
    echo "${out}"
    grep -q 'all checks passed' <<<"${out}"
}

# A GET sweep at the three sizes ci/report/publish-perf.py
# tracks, written as a bandwidth CSV so gen-report.py picks
# it up off the shared results dir with no schema change --
# the header the client emits is the tuple it already
# dispatches on, and verb "s3" is what keeps these rows
# apart from the perftest "send" and "nvmeof" ones.
#
# GETs, not PUTs: a GET has the store RDMA-WRITE into guest
# memory, which is the direction the badge speaks for.  The
# write direction is already proven by probe_functional.
probe_perf_sweep() {
    local n="$1" out ts csv

    out="$(vm_ssh "${n}" "cd ${GUEST_DIR} && timeout 300 \
        ./s3_rdma_client -a ${S3_ADDR} -p ${S3_PORT} -b ${S3_BUCKET} \
        -s ${S3_BUFFER} -i ${S3_PERF_ITERS} -P -c ${GUEST_DIR}/s3-bw.csv" \
        2>&1)" || {
        echo "${out}"
        log_error "guest ${n}: the S3 sweep failed"
        return 1
    }
    echo "${out}"

    # Numbers measured under TCG are one to two orders of
    # magnitude off and would poison the trend history, so
    # the sweep still runs as a check but its CSV is only
    # recorded under KVM.  perf.sh makes the same refusal.
    if [ "${CI_VM_ACCEL:-kvm}" != "kvm" ]; then
        log_warn "CI_VM_ACCEL=${CI_VM_ACCEL:-kvm}, not kvm;" \
                 "not recording emulated S3 numbers"
        return 0
    fi

    mkdir -p "${CI_RESULTS}/perf-csv"
    ts="$(date +%Y-%m-%d-%H%M%S)"
    csv="${CI_RESULTS}/perf-csv/s3-bw-${ts}.csv"
    vm_ssh "${n}" "cat ${GUEST_DIR}/s3-bw.csv" >"${csv}" 2>/dev/null || {
        log_error "guest ${n}: could not retrieve the sweep CSV"
        rm -f "${csv}"
        return 1
    }
    # A header with no rows under it is an empty sweep, which
    # would otherwise be published as a gap in the trend
    # rather than as the failure it is.
    if [ "$(grep -c '^s3,' "${csv}")" -eq 0 ]; then
        log_error "guest ${n}: the sweep CSV has no rows"
        cat "${csv}"
        rm -f "${csv}"
        return 1
    fi
    log_info "guest ${n}: S3 sweep recorded in ${csv}"
}

# The store's own accounting, read back through the control
# plane: the objects the functional run left behind were
# deleted, so a non-empty listing means a DELETE reported
# success without doing anything.
probe_store_empty() {
    local out
    out="$(vm_ssh "$1" "curl -sS --max-time 20 \
        http://${S3_ADDR}:${S3_PORT}/${S3_BUCKET}/" 2>&1)"
    echo "${out}"
    grep -q '<KeyCount>0</KeyCount>' <<<"${out}"
}

# ── Run ───────────────────────────────────────────

for i in $(seq 1 "${ERNIC_INSTANCES}"); do
    group_start "Guest ${i}: S3 over RDMA against the in-process store"

    run_check vm-s3 "vm-${i}-nic-address" guest_nic "${i}"
    run_check vm-s3 "vm-${i}-endpoint-up" probe_endpoint_up "${i}"
    run_check vm-s3 "vm-${i}-http-control" probe_http_control "${i}"
    run_check vm-s3 "vm-${i}-build-client" build_client "${i}"
    run_check vm-s3 "vm-${i}-functional" probe_functional "${i}"
    run_check vm-s3 "vm-${i}-store-empty" probe_store_empty "${i}"
    run_check vm-s3 "vm-${i}-perf-sweep" probe_perf_sweep "${i}"

    group_end
done

# A server that died mid-run invalidates every result above
# it, so name it rather than leaving it to be inferred.
group_start "Instance liveness"
run_check vm-s3 "instances-survived" require_instances_alive
group_end

log_info "vm-s3 job complete; results in ${CI_RESULTS}/vm-s3.jsonl"
