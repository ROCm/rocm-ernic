#!/usr/bin/env bash
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Run include-what-you-use (IWYU) and record the run
#
# CMake only shows IWYU's output when IWYU suggests a change, and it
# ignores IWYU's exit status, so a run that fails (for example, because
# IWYU's clang can't parse a file) leaves no trace in the build log and
# doesn't fail the build. To catch that, point CMake at this script
# instead of at IWYU, and set ERNIC_IWYU_LOG_DIR to an existing
# directory when building:
#
#   cmake -B build -DERNIC_USE_IWYU=ON -DERNIC_IWYU_EXE="$PWD/ci/iwyu/iwyu-wrapper.sh"
#   mkdir build/iwyu-runs
#   ERNIC_IWYU_LOG_DIR="$PWD/build/iwyu-runs" cmake --build build
#   ci/iwyu/check-iwyu.sh build/iwyu-runs
#
# For each run, this saves the arguments, output, and exit status in
# ERNIC_IWYU_LOG_DIR for check-iwyu.sh to check. IWYU's output and exit
# status are passed through, so CMake behaves as it does without this
# script.
#
# ERNIC_IWYU_WRAPPED_EXE selects the IWYU to run. It defaults to the
# include-what-you-use on PATH.

set -u

log_dir=${ERNIC_IWYU_LOG_DIR:?ERNIC_IWYU_LOG_DIR must be set}
iwyu=${ERNIC_IWYU_WRAPPED_EXE:-include-what-you-use}

# The run's exit status goes in this file. Its name is the base for the
# run's other files.
run=$(mktemp "${log_dir}/run.XXXXXX") || exit 1

printf '%q ' "${iwyu}" "$@" > "${run}.cmd"
echo >> "${run}.cmd"

"${iwyu}" "$@" > "${run}.stdout" 2> "${run}.stderr"
status=$?
echo "${status}" > "${run}"

cat "${run}.stdout"
cat "${run}.stderr" >&2
exit "${status}"
