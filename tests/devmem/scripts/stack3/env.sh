# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Sourced by the stack 3 end-to-end scripts.  Stack 3 is the ROCm 10.2 guest
# image (compose project ionic-devmem-r102, guests r102-1 and r102-2).
#
# Stack 3's own wrappers -- its env.sh, which sets PROJECT and ETH_SUBNET,
# and gssh and gscp -- are NOT in this repository: see ../../README.md,
# "The harness".  $STACK3 below is where they live in the work area.
# Without them these scripts cannot run; they are here to be read, adapted
# and repeated, not as a supported harness.
#
# A script that sources this keeps its own directory in HERE: this file and
# the one it sources may set anything else.
#
# shellcheck shell=bash

: "${DEVMEM_ROOT:?set it to the work area}"
STACK3=$DEVMEM_ROOT/test-rocm102/scripts
E2E=$DEVMEM_ROOT/test-rocm102/e2e
# EV, IF and the two functions are for the scripts that source this.
# shellcheck disable=SC2034  # used by case.sh, sizes.sh and bench.sh
EV=$E2E/evidence
IF=${IF:-rocm-ernic0}
# shellcheck source=/dev/null
. "$STACK3/env.sh" # PROJECT, ETH_SUBNET

g() { "$STACK3/gssh" "$@"; }
gscp() { "$STACK3/gscp" "$@"; }
