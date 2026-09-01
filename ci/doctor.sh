#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# ci/doctor.sh -- preflight check for the self-hosted CI
# node.
#
# Reports what each CI tier needs and whether this node
# can satisfy it.  Exit status:
#   0  everything needed for the full matrix is present
#   1  build/loopback tier is fine, VM or perf tier is not
#   2  the node cannot run CI at all
#
# Run this first when a job starts failing for
# environmental reasons.

# shellcheck source=/dev/null
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

PASS=0; WARN=0; FAIL=0

ok()   { echo "  [ ok ] $*"; PASS=$((PASS+1)); }
warn() { echo "  [warn] $*"; WARN=$((WARN+1)); }
bad()  { echo "  [FAIL] $*"; FAIL=$((FAIL+1)); }

have() { command -v "$1" >/dev/null 2>&1; }

echo "rocm-ernic CI doctor -- $(hostname -f)"
echo

# ── Tier 1: build, ctest, loopback ────────────────

echo "Tier 1: build / ctest / loopback"
for tool in cmake ninja pkg-config gcc python3 ansible-playbook; do
    if have "$tool"; then
        ok "$tool: $(command -v "$tool")"
    else
        bad "$tool not found"
    fi
done

for mod in libibverbs librdmacm glib-2.0 json-c cmocka; do
    if pkg-config --exists "$mod" 2>/dev/null; then
        ok "$mod: $(pkg-config --modversion "$mod")"
    else
        bad "$mod development files missing"
    fi
done

# CMake falls back to a direct library search when the
# .pc file is absent, so a missing pkg-config entry here
# is not fatal.
if pkg-config --exists vfio-user 2>/dev/null; then
    ok "vfio-user: $(pkg-config --modversion vfio-user)"
elif ls /usr/lib/x86_64-linux-gnu/libvfio-user.so \
        /usr/local/lib/x86_64-linux-gnu/libvfio-user.so \
        >/dev/null 2>&1; then
    warn "libvfio-user present but no pkg-config file (CMake will use its fallback)"
else
    bad "libvfio-user not found"
fi

for f in "${ERNIC_LAUNCHER}" "${ERNICCTL}"; do
    if [ -x "$f" ]; then ok "$(basename "$f"): $f"
    else bad "$(basename "$f") missing or not executable: $f"; fi
done

if [ -w "$(dirname "${CI_WORK}")" ] || [ -w "${CI_WORK}" ]; then
    ok "workspace writable: ${CI_WORK}"
else
    bad "workspace not writable: ${CI_WORK}"
fi
echo

# ── Tier 2: VM functional ─────────────────────────

echo "Tier 2: 2-VM RDMA functional"
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ok "/dev/kvm accessible"
else
    kvm_gid="$(stat -c '%g' /dev/kvm 2>/dev/null || echo '?')"
    real_gid="$(getent group kvm | cut -d: -f3)"
    bad "/dev/kvm not accessible (owned by gid ${kvm_gid}; kvm group is ${real_gid}; you are in $(id -Gn | tr ' ' ','))"
    echo "         fix: sudo chgrp kvm /dev/kvm && sudo chmod 660 /dev/kvm"
    echo "         or:  run VM jobs with CI_VM_ACCEL=tcg (functional only, no valid perf)"
fi

if [ -f "${CI_VM_BACKING}" ]; then
    ok "golden backing image: ${CI_VM_BACKING} ($(du -h "${CI_VM_BACKING}" | cut -f1))"
else
    bad "golden backing image missing: ${CI_VM_BACKING}"
    echo "         build one with: ansible-playbook ansible/playbooks/vm-create.yml (needs root)"
fi

if [ -x "${CI_QEMU_PATH}/qemu-system-x86_64" ]; then
    ok "qemu: ${CI_QEMU_PATH}qemu-system-x86_64"
elif have qemu-system-x86_64; then
    warn "custom qemu not at ${CI_QEMU_PATH}; falling back to $(command -v qemu-system-x86_64)"
else
    bad "no qemu-system-x86_64 found"
fi

if [ -f "${CI_QEMU_MINIMAL}/qemu/run-vm" ]; then
    ok "qemu-minimal: ${CI_QEMU_MINIMAL}"
else
    bad "qemu-minimal run-vm not found at ${CI_QEMU_MINIMAL}"
fi

if [ -w "${CI_VM_IMAGE_DIR}" ]; then
    ok "image dir writable: ${CI_VM_IMAGE_DIR}"
else
    bad "image dir not writable: ${CI_VM_IMAGE_DIR}"
fi
echo

# ── Tier 3: performance ───────────────────────────

echo "Tier 3: performance sweeps"
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ok "KVM available (perf numbers will be meaningful)"
else
    bad "perf requires KVM; emulated numbers are rejected by ci/jobs/perf.sh"
fi

avail_gb=$(df -BG --output=avail "${CI_WORK%/*}" 2>/dev/null | tail -1 | tr -dc '0-9')
if [ "${avail_gb:-0}" -ge 50 ]; then
    ok "free space: ${avail_gb}G on $(df --output=target "${CI_WORK%/*}" | tail -1)"
else
    warn "only ${avail_gb:-?}G free; VM overlays and results need headroom"
fi
echo

# ── Runner ────────────────────────────────────────

echo "Runner"
RUNNER_ROOT="${RUNNER_ROOT:-${CI_RUNNER_ROOT:-/local/${USER}/actions-runner-rocm-ernic}}"
if [ -x "${RUNNER_ROOT}/config.sh" ]; then
    ok "runner staged at ${RUNNER_ROOT}"
    if [ -f "${RUNNER_ROOT}/.runner" ]; then
        # The runner writes .runner with a UTF-8 BOM, which
        # plain utf-8 decoding rejects; without encoding=
        # this silently reported '?' for a healthy runner.
        ok "runner registered: $(python3 -c "
import json
with open('${RUNNER_ROOT}/.runner', encoding='utf-8-sig') as fh:
    print(json.load(fh).get('gitHubUrl', '?'))" 2>/dev/null || echo '?')"
    else
        warn "runner staged but not registered -- see ci/runner/register-runner.sh"
    fi
else
    warn "runner not installed -- see ci/runner/install-runner.sh"
fi

if systemctl --user is-active --quiet rocm-ernic-runner.service 2>/dev/null; then
    ok "runner service active"
else
    warn "runner service not active"
fi

if [ "$(loginctl show-user "$(id -un)" -p Linger --value 2>/dev/null)" = "yes" ]; then
    ok "lingering enabled (runner survives reboot)"
else
    warn "lingering disabled; runner will not auto-start at boot"
fi
echo

echo "Summary: ${PASS} ok, ${WARN} warning(s), ${FAIL} failure(s)"

if [ "${FAIL}" -gt 0 ]; then
    # Distinguish "cannot build at all" from "cannot do VMs".
    if have cmake && have ninja && [ -x "${ERNIC_LAUNCHER}" ]; then
        exit 1
    fi
    exit 2
fi
exit 0
