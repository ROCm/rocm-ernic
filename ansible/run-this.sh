#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#

# Ansible fails if LANG points at an ungenerated locale (e.g. en_US.UTF-8).
export LANG=C.UTF-8
export LC_ALL=C.UTF-8

if ! ansible-playbook site.yml \
    --tags host-setup,vm-create,guest-setup \
    -e ernic_force_vm_cleanup=true; then
  echo "=== Setup failed; fix errors above before running perf loop ==="
  exit 1
fi

PASS=0
while ansible-playbook site.yml \
    --tags tcp-perf,gpu-perftest-xio \
    --skip-tags stress,gpu-test; do
  PASS=$((PASS + 1))
  echo "=== Pass $PASS completed at $(date), restarting ==="
done
echo "=== FAILED on pass $((PASS + 1)) at $(date) ==="
