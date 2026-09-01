#!/bin/bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# ci/jobs/build.sh -- configure, build and unit-test
# rocm-ernic on the self-hosted runner.
#
# Emits:
#   $CI_RESULTS/build.jsonl        per-check results
#   $CI_RESULTS/ctest-junit.xml    CTest JUnit output
#
# Env:
#   CI_BUILD_TYPE   Release (default) | Debug | ASan
#   CI_BUILD_JOBS   ninja parallelism (default: nproc)

# shellcheck source=/dev/null
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/common.sh"

CI_BUILD_TYPE="${CI_BUILD_TYPE:-Release}"
CI_BUILD_JOBS="${CI_BUILD_JOBS:-$(nproc)}"

mkdir -p "${CI_RESULTS}"

group_start "Configure (${CI_BUILD_TYPE})"
cmake_args=(
    -S "${PROJECT_ROOT}"
    -B "${CI_BUILD_DIR}"
    -G Ninja
    -DCMAKE_BUILD_TYPE="${CI_BUILD_TYPE}"
)
# ERNIC_WERROR is off by default upstream; CI should be
# strict about new warnings.
cmake_args+=(-DERNIC_WERROR=ON)

run_check build "cmake-configure" cmake "${cmake_args[@]}"
group_end

group_start "Build"
run_check build "ninja-build" \
    cmake --build "${CI_BUILD_DIR}" -- -j "${CI_BUILD_JOBS}"
group_end

# A build failure makes every downstream check
# meaningless, so bail out rather than reporting a
# cascade of misleading failures.
if ! [ -x "${CI_BUILD_DIR}/rocm-ernic" ]; then
    die "build produced no rocm-ernic binary; aborting"
fi

group_start "CTest"
# --output-junit gives the workflow a structured
# artifact; run_check still records the aggregate.
run_check build "ctest" \
    ctest --test-dir "${CI_BUILD_DIR}" \
          --output-on-failure \
          --output-junit "${CI_RESULTS}/ctest-junit.xml"
group_end

group_start "Binary sanity"
run_check build "binary-executable" \
    test -x "${CI_BUILD_DIR}/rocm-ernic"
run_check build "version-string" \
    bash -c "'${CI_BUILD_DIR}/rocm-ernic' --help 2>&1 | head -1 | grep -q ."
group_end

log_info "build job complete; results in ${CI_RESULTS}/build.jsonl"
