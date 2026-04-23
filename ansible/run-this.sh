#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#

ansible-playbook site.yml \
    --tags host-setup,vm-create,guest-setup \
    -e ernic_force_vm_cleanup=true

PASS=0
while ansible-playbook site.yml \
    --tags sanity,tcp-perf \
    --skip-tags stress,gpu-test; do
  PASS=$((PASS + 1))
  echo "=== Pass $PASS completed at $(date), restarting ==="
done
echo "=== FAILED on pass $((PASS + 1)) at $(date) ==="
