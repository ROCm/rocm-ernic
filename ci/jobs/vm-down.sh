#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# ci/jobs/vm-down.sh -- tear down CI VMs and instances.
#
# Always exits 0: this runs from an `if: always()` step
# and must never be the reason a run is marked failed.
# Deliberately scoped to CI_VM_NAME_BASE so it can
# never touch a developer's VMs on the same box.

# shellcheck source=/dev/null
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

set +e
ernic_env

group_start "Stop CI VMs"
for i in $(seq 1 "${ERNIC_INSTANCES}"); do
    log_info "stopping VM ${i}"
    timeout 60 "${ERNICCTL}" vm-stop "${i}" 2>&1 | tail -2
done
group_end

group_start "Stop rocm-ernic instances"
timeout 60 "${ERNIC_LAUNCHER}" --stop 2>&1 | tail -3
group_end

group_start "Reap stragglers"
# vm-stop goes through QMP, which can time out if a
# guest wedged.  Match on the CI VM name so this only
# ever reaps VMs this harness launched.
for i in $(seq 1 "${ERNIC_INSTANCES}"); do
    pkill -f "qemu-system.*${CI_VM_NAME_BASE}-${i}" 2>/dev/null \
        && log_warn "force-killed qemu for ${CI_VM_NAME_BASE}-${i}"
done
group_end

if [ "${CI_KEEP_OVERLAYS:-false}" != "true" ]; then
    group_start "Remove CI overlays"
    for i in $(seq 1 "${ERNIC_INSTANCES}"); do
        overlay="${CI_VM_IMAGE_DIR}/${CI_VM_NAME_BASE}-${i}.qcow2"
        if [ -f "${overlay}" ]; then
            rm -f "${overlay}" && log_info "removed ${overlay}"
        fi
    done
    group_end
fi

log_info "teardown complete"
exit 0
