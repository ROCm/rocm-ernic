#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# ci/jobs/loopback.sh -- host-side functional tests that
# need no VM and no KVM.
#
# Covers:
#   * the six loopback backend configurations exercised
#     by tests/test_loopback_ci.sh
#   * vfio-user socket bring-up for the PCI client
#   * the multi-instance launcher / ernicctl control
#     plane, run entirely unprivileged
#
# Emits: $CI_RESULTS/loopback.jsonl

# shellcheck source=/dev/null
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

ernic_env
mkdir -p "${CI_RESULTS}" "${CI_RUN_DIR}" "${CI_LOG_DIR}"

[ -x "${CI_BUILD_DIR}/rocm-ernic" ] || \
    die "no build at ${CI_BUILD_DIR}; run ci/jobs/build.sh first"

cleanup() {
    log_info "tearing down loopback instances"
    "${ERNIC_LAUNCHER}" --stop >/dev/null 2>&1 || true
}
trap cleanup EXIT

# ── Loopback backend configurations ───────────────

group_start "Loopback backend configurations"
run_check loopback "loopback-configs" \
    env BUILD_DIR="${CI_BUILD_DIR}" \
        SERVER_BIN="${CI_BUILD_DIR}/rocm-ernic" \
        TEST_BIN="${CI_BUILD_DIR}/tests/test_data_transfer" \
        bash "${PROJECT_ROOT}/tests/test_loopback_ci.sh"
group_end

# ── vfio-user PCI client ──────────────────────────

group_start "vfio-user PCI client"
run_check loopback "pci-client" \
    bash "${PROJECT_ROOT}/tests/run-test.sh" \
        "${CI_BUILD_DIR}/tests/test_pci_client" \
        "${CI_BUILD_DIR}/rocm-ernic"
group_end

# ── Multi-instance control plane ──────────────────
#
# This is the same launcher the systemd unit drives,
# but pointed at CI_RUN_DIR / CI_LOG_DIR so it needs no
# root.  Exercising it here catches manifest and socket
# regressions before the VM jobs depend on them.

group_start "Multi-instance control plane"

"${ERNIC_LAUNCHER}" --stop >/dev/null 2>&1 || true

run_check loopback "launcher-start" \
    "${ERNIC_LAUNCHER}"

run_check loopback "manifest-written" \
    test -s "${CI_RUN_DIR}/instances.json"

run_check loopback "manifest-instance-count" \
    bash -c "python3 -c \"
import json,sys
m=json.load(open('${CI_RUN_DIR}/instances.json'))
n=len(m['instances'])
sys.exit(0 if n==${ERNIC_INSTANCES} else 1)\""

for i in $(seq 1 "${ERNIC_INSTANCES}"); do
    run_check loopback "instance-${i}-socket" \
        test -S "${CI_RUN_DIR}/${i}.sock"
done

run_check loopback "ernicctl-status" \
    bash -c "'${ERNICCTL}' status | grep -q running"

run_check loopback "launcher-stop" \
    "${ERNIC_LAUNCHER}" --stop

group_end

log_info "loopback job complete; results in ${CI_RESULTS}/loopback.jsonl"
