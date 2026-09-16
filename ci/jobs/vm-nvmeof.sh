#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# ci/jobs/vm-nvmeof.sh -- single-VM NVMe-oF functional
# test over the emulated NIC.
#
# Unlike vm-functional.sh this needs no peer: each
# rocm-ernic instance runs its own in-process NVMe-oF
# controller, so one guest and one server are a complete
# fabric.  Bring the lane up with:
#
#   ERNIC_INSTANCES=1 ERNIC_BACKEND=nvmeof:size=256M,bs=4096 \
#       bash ci/jobs/vm-up.sh
#   bash ci/jobs/vm-nvmeof.sh
#
# The guest steps mirror the nvmeof_setup role in
# batesste-ansible (host mode): load nvme-rdma, discover,
# connect, use the namespace, disconnect.
#
# ansible/playbooks/nvmeof-tests.yml checks the same things
# and is what the hosted system-test-nvmeof job runs.  This
# stays shell because each check has to land in the CI
# report as its own JSONL record, the same split the repo
# already has between sanity-tests.yml and vm-functional.sh.
# Keep the two in step.
#
# Emits: $CI_RESULTS/vm-nvmeof.jsonl

# shellcheck source=/dev/null
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

ernic_env
mkdir -p "${CI_RESULTS}"
start_suite vm-nvmeof

# The controller's address and NQN have to match what the
# instances were started with; both default to the server's
# own defaults, which sit on the same subnet the guest NIC
# is addressed from (ernic_nic_subnet in group_vars).
NVMEOF_TRADDR="${NVMEOF_TRADDR:-192.168.200.1}"
NVMEOF_TRSVCID="${NVMEOF_TRSVCID:-4420}"
NVMEOF_NQN="${NVMEOF_NQN:-nvmet-test}"
# Any address will do: the emulator routes the CM exchange by
# QP, not by MAC.  The entry only exists so the guest's
# rdma_resolve_addr() has a neighbour to resolve to -- nothing
# on the wire answers ARP for the controller.
NVMEOF_LLADDR="${NVMEOF_LLADDR:-02:00:00:00:c0:01}"

case "${ERNIC_BACKEND:-}" in
nvmeof*) ;;
*)
    log_warn "ERNIC_BACKEND is '${ERNIC_BACKEND:-<unset>}', not nvmeof;"
    log_warn "the instances are not running a controller to connect to"
    ;;
esac

# ── Guest provisioning ────────────────────────────
#
# vm-down.sh deletes the qcow2 overlay at the end of every
# job, so the VM vm-up.sh just booted is a pristine clone of
# the golden image: no ionic_rdma built for the running
# kernel, no nvme-cli, and no address on rocm-ernic0 -- and
# without that address the driver populates no RoCEv2 GID,
# so rdma_resolve_addr() has nothing to bind to.  Every
# check below depends on this, which is why it runs first
# and why the job stops here if it fails.

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
        -e "ernic_golden_image=false" \
        -e "ernic_build=false" \
        -e "ernic_gpu_passthrough=${CI_GPU_PASSTHROUGH}" \
        "$@" </dev/null
}

[ -f "${ANSIBLE_DIR}/ci-site.yml" ] || \
    die "ci-site.yml not found under ${ANSIBLE_DIR}"

cd "${ANSIBLE_DIR}" || die "cannot cd to ${ANSIBLE_DIR}"

group_start "Guest setup (driver + rdma-core + NIC address)"
run_check vm-nvmeof "guest-setup" ci_ansible --tags guest-setup
run_check vm-nvmeof "guests-ready" require_guests_ready
group_end

# ── Guest-side helpers ────────────────────────────

guest_nic() {
    vm_ssh "$1" 'ip -4 -br addr show rocm-ernic0' 2>/dev/null \
        | grep -qE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+'
}

load_modules() {
    vm_ssh "$1" 'sudo modprobe nvme-fabrics && sudo modprobe nvme-rdma' \
        >/dev/null
}

# rdma_resolve_addr() turns the target IP into a route and a
# neighbour before the CM exchange starts.  The controller is
# not a host on the segment, so seed the entry by hand.
seed_neighbour() {
    vm_ssh "$1" "sudo ip neigh replace ${NVMEOF_TRADDR} \
        lladdr ${NVMEOF_LLADDR} dev rocm-ernic0 nud permanent" >/dev/null
}

nvme_discover() {
    local out
    out="$(vm_ssh "$1" "sudo nvme discover -t rdma \
        -a ${NVMEOF_TRADDR} -s ${NVMEOF_TRSVCID}" 2>&1)"
    echo "${out}"
    grep -q "subnqn:[[:space:]]*${NVMEOF_NQN}" <<<"${out}"
}

nvme_connect() {
    vm_ssh "$1" "sudo nvme connect -t rdma \
        -a ${NVMEOF_TRADDR} -s ${NVMEOF_TRSVCID} -n ${NVMEOF_NQN}" \
        >/dev/null
}

# The block device the connect produced, e.g. /dev/nvme1n1.
guest_ns_dev() {
    vm_ssh "$1" "sudo nvme list -o json 2>/dev/null \
        | python3 -c \"
import json,sys
d=json.load(sys.stdin)
for dev in d.get('Devices', []):
    path = dev.get('DevicePath') or dev.get('NameSpace')
    if dev.get('ModelNumber','').startswith('rocm-ernic'):
        print(path)
        break
\"" | tr -d '\r'
}

# nvme list's model filter above is the fast path; fall back to
# matching the subsystem NQN in sysfs, which works regardless of how
# the model string is spelled.  Not "nvme list-subsys ${NQN}": that
# positional is a device, so nvme-cli 2.x answers "Invalid device
# name nvmet-test" and exits 1, which made the fallback yield the
# empty string always -- and probe_gone, defined as "resolve_ns is
# empty", pass unconditionally.
guest_ns_dev_by_subsys() {
    vm_ssh "$1" "for s in /sys/class/nvme/nvme*; do
        [ -r \"\$s/subsysnqn\" ] || continue
        [ \"\$(cat \"\$s/subsysnqn\")\" = '${NVMEOF_NQN}' ] || continue
        basename \"\$s\"
        break
    done" 2>/dev/null | tr -d '\r'
}

resolve_ns() {
    local n="$1" dev
    dev="$(guest_ns_dev "${n}")"
    if [ -z "${dev}" ]; then
        local ctrl
        ctrl="$(guest_ns_dev_by_subsys "${n}")"
        [ -n "${ctrl}" ] && dev="/dev/${ctrl}n1"
    fi
    echo "${dev}"
}

probe_namespace_present() {
    local dev
    dev="$(resolve_ns "$1")"
    if [ -z "${dev}" ]; then
        log_error "guest $1: no namespace appeared for ${NVMEOF_NQN}"
        vm_ssh "$1" 'sudo nvme list; sudo nvme list-subsys' 2>&1 || true
        return 1
    fi
    log_info "guest $1: namespace ${dev}"
    vm_ssh "$1" "sudo nvme id-ctrl ${dev} | head -20" 2>&1 || true
    vm_ssh "$1" "test -b ${dev}"
}

# Write a known pattern, drop the page cache, read it back and
# compare digests.  This is the check that proves the capsules
# and the RDMA transfers behind them actually moved the bytes.
probe_data_integrity() {
    local n="$1" dev out
    dev="$(resolve_ns "${n}")"
    [ -n "${dev}" ] || return 1

    out="$(vm_ssh "${n}" "set -e
        sudo dd if=/dev/urandom of=/tmp/nvmeof-src.bin bs=1M count=16 \
            status=none
        src=\$(md5sum /tmp/nvmeof-src.bin | awk '{print \$1}')
        sudo dd if=/tmp/nvmeof-src.bin of=${dev} bs=1M oflag=direct status=none
        sudo dd if=${dev} of=/tmp/nvmeof-dst.bin bs=1M count=16 \
            iflag=direct status=none
        dst=\$(md5sum /tmp/nvmeof-dst.bin | awk '{print \$1}')
        echo \"src=\$src dst=\$dst\"
        test \"\$src\" = \"\$dst\"
        sudo rm -f /tmp/nvmeof-src.bin /tmp/nvmeof-dst.bin" 2>&1)"
    echo "${out}"
    grep -qE 'src=([0-9a-f]+) dst=\1' <<<"${out}"
}

probe_fio() {
    local n="$1" dev out
    dev="$(resolve_ns "${n}")"
    [ -n "${dev}" ] || return 1

    # A skip must not read as a pass: run_check has no skip
    # state, so a silently absent fio would land in the report
    # as a green check that never ran.  The golden image is
    # supposed to carry fio, so its absence is a failure.
    if ! vm_ssh "${n}" 'command -v fio' >/dev/null 2>&1; then
        echo "fio is not installed in guest ${n}"
        return 1
    fi

    out="$(vm_ssh "${n}" "sudo fio --name=nvmeof --filename=${dev} \
        --rw=randrw --bs=4k --size=32M --iodepth=8 --ioengine=libaio \
        --direct=1 --runtime=20 --time_based --group_reporting \
        --output-format=normal" 2>&1)"
    echo "${out}"
    grep -q 'IOPS=' <<<"${out}"
}

# A random-read sweep at the three sizes ci/report/publish-perf.py
# tracks, written as a bandwidth CSV so gen-report.py picks it up off
# the shared results dir with no schema change -- the header below is
# the tuple it already dispatches on, and verb "nvmeof" is what keeps
# these rows apart from the perftest "send" ones.
#
# Reads, not the functional randrw above: a read has the controller
# RDMA-WRITE into guest memory, which is the path the badge is meant
# to speak for, and mixing the two makes both numbers mean half of
# something.  The write direction is already proven by
# probe_data_integrity.
NVMEOF_PERF_BS="${NVMEOF_PERF_BS:-4k 64k 1M}"

# Reads one fio --output-format=json document on stdin, takes the
# block size as argv[1], writes one bandwidth CSV row.
read -r -d '' NVMEOF_FIO_TO_CSV <<'PY' || true
import json, sys
bs = sys.argv[1]
mult = {"k": 1024, "K": 1024, "m": 1048576, "M": 1048576}
size = int(bs[:-1]) * mult[bs[-1]] if bs[-1] in mult else int(bs)
r = json.load(sys.stdin)["jobs"][0]["read"]
# fio reports bw and bw_max in KiB/s, bw_bytes in bytes/s.
print("nvmeof,%d,%.6f,%.6f,%.6f" % (
    size,
    r.get("bw_max", 0) * 1024 / 1e9,
    r.get("bw_bytes", 0) / 1e9,
    r.get("iops", 0) / 1e6))
PY

probe_fio_sweep() {
    local n="$1" dev bs json ts csv rows row

    # Numbers measured under TCG are one to two orders of magnitude
    # off and would poison the trend history, so the sweep still runs
    # as a check but its CSV is only recorded under KVM.  perf.sh
    # makes the same refusal.
    dev="$(resolve_ns "${n}")"
    [ -n "${dev}" ] || return 1

    rows=""
    # shellcheck disable=SC2086  # a deliberate word split of the list
    for bs in ${NVMEOF_PERF_BS}; do
        json="$(vm_ssh "${n}" "sudo fio --name=nvmeof-${bs} \
            --filename=${dev} --rw=randread --bs=${bs} \
            --size=${NVMEOF_PERF_SIZE:-64M} --iodepth=32 \
            --ioengine=libaio --direct=1 \
            --runtime=${NVMEOF_PERF_RUNTIME:-10} --time_based \
            --group_reporting --output-format=json" 2>/dev/null)" || {
            log_error "guest ${n}: fio sweep failed at bs=${bs}"
            return 1
        }
        row="$(python3 -c "${NVMEOF_FIO_TO_CSV}" "${bs}" \
            <<<"${json}")" || {
            log_error "guest ${n}: could not parse fio json at bs=${bs}"
            return 1
        }
        rows+="${row}"$'\n'
    done

    printf '%s' "${rows}"

    if [ "${CI_VM_ACCEL:-kvm}" != "kvm" ]; then
        log_warn "CI_VM_ACCEL=${CI_VM_ACCEL:-kvm}, not kvm;" \
                 "not recording emulated NVMe-oF numbers"
        return 0
    fi

    mkdir -p "${CI_RESULTS}/perf-csv"
    ts="$(date +%Y-%m-%d-%H%M%S)"
    csv="${CI_RESULTS}/perf-csv/nvmeof-bw-${ts}.csv"
    {
        echo "verb,size,bw_peak_GBs,bw_avg_GBs,msg_rate_mpps"
        printf '%s' "${rows}"
    } >"${csv}"
    log_info "guest ${n}: NVMe-oF sweep recorded in ${csv}"
}

# nvme-cli prints "NQN:%s disconnected %d controller(s)" and exits 0
# even when the count is zero, so a bare grep for 'disconnected'
# passes when nothing was ever connected.  Require a non-zero count.
probe_disconnect() {
    local out
    out="$(vm_ssh "$1" "sudo nvme disconnect -n ${NVMEOF_NQN}" 2>&1)"
    echo "${out}"
    grep -qE 'disconnected [1-9][0-9]* controller' <<<"${out}"
}

probe_gone() {
    [ -z "$(resolve_ns "$1")" ]
}

# ── Run ───────────────────────────────────────────

for i in $(seq 1 "${ERNIC_INSTANCES}"); do
    group_start "Guest ${i}: NVMe-oF over the emulated fabric"

    run_check vm-nvmeof "vm-${i}-nic-address" guest_nic "${i}"
    run_check vm-nvmeof "vm-${i}-nvme-rdma-modules" load_modules "${i}"
    run_check vm-nvmeof "vm-${i}-neighbour" seed_neighbour "${i}"
    run_check vm-nvmeof "vm-${i}-discover" nvme_discover "${i}"
    run_check vm-nvmeof "vm-${i}-connect" nvme_connect "${i}"
    run_check vm-nvmeof "vm-${i}-namespace" probe_namespace_present "${i}"
    run_check vm-nvmeof "vm-${i}-data-integrity" probe_data_integrity "${i}"
    run_check vm-nvmeof "vm-${i}-fio" probe_fio "${i}"
    run_check vm-nvmeof "vm-${i}-fio-sweep" probe_fio_sweep "${i}"
    run_check vm-nvmeof "vm-${i}-disconnect" probe_disconnect "${i}"
    run_check vm-nvmeof "vm-${i}-namespace-gone" probe_gone "${i}"

    group_end
done

# A server that died mid-run invalidates every result above
# it, so name it rather than leaving it to be inferred.
group_start "Instance liveness"
run_check vm-nvmeof "instances-survived" require_instances_alive
group_end

log_info "vm-nvmeof job complete; results in ${CI_RESULTS}/vm-nvmeof.jsonl"
