#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Enables kernel.softlockup_all_cpu_backtrace on both guests (so the next
# soft lockup dumps all CPU stacks to the guest console / serial log), then
# runs run-iperf3.sh.  After a hang, inspect /var/log/rocm-ernic/vm-2.log on
# the host for "Call Trace" / "softlockup".

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

SSH_BASE=(
  ssh
  -o BatchMode=yes
  -o StrictHostKeyChecking=no
  -o UserKnownHostsFile=/dev/null
  -o ConnectTimeout=15
)

enable_softlock_trace() {
  local port="$1"
  local label="$2"
  echo "=== ${label} (port ${port}): kernel.softlockup_all_cpu_backtrace ==="
  if ! timeout 45 "${SSH_BASE[@]}" -p "${port}" ubuntu@localhost \
    'sudo sysctl -w kernel.softlockup_all_cpu_backtrace=1 && cat /proc/sys/kernel/softlockup_all_cpu_backtrace'; then
    echo "run-iperf3-softlock-trace.sh: ${label}: SSH or sysctl failed" >&2
    exit 1
  fi
  echo ""
}

enable_softlock_trace 2250 VM1
enable_softlock_trace 2251 VM2

exec "${SCRIPT_DIR}/run-iperf3.sh"
