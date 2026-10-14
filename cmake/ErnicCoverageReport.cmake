# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Write the code coverage report (run by the ernic-coverage target; see
# ErnicCoverage.cmake)
#
# Variables:
#   LLVM_PROFDATA   Path to llvm-profdata
#   LLVM_COV        Path to llvm-cov
#   COVERAGE_DIR    The directory that holds the profiles (in profraw/)
#                   and gets the report
#   SOURCE_DIR      The sources to report on
#   BINARIES        The instrumented executables

cmake_minimum_required(VERSION 3.21)

foreach(var IN ITEMS LLVM_PROFDATA LLVM_COV COVERAGE_DIR SOURCE_DIR BINARIES)
    if(NOT ${var})
        message(FATAL_ERROR "ErnicCoverageReport: ${var} is not set")
    endif()
endforeach()

file(GLOB profiles "${COVERAGE_DIR}/profraw/*.profraw")
if(NOT profiles)
    message(FATAL_ERROR
      "No coverage profiles in ${COVERAGE_DIR}/profraw. Run the tests first.")
endif()

# Many of the sources are compiled into more than one binary, and the
# unit tests that #include a translation unit compile it against their
# own stubs, so llvm-cov warns that some functions have mismatched data:
# the profiles have another copy of the function, but not the copy in
# that binary, which didn't run. -sparse leaves out functions that never
# ran, so the copies that did run are the ones reported.
set(profdata "${COVERAGE_DIR}/ernic.profdata")
execute_process(
  COMMAND "${LLVM_PROFDATA}" merge -sparse -o "${profdata}" ${profiles}
  COMMAND_ERROR_IS_FATAL ANY)

# If DEBUGINFOD_URLS is set, as it is by default on Ubuntu, llvm-cov
# tries to download each binary in the profiles that it isn't given,
# such as the binaries from a build that was replaced since the profiles
# were written
unset(ENV{DEBUGINFOD_URLS})

# llvm-cov takes the first binary as an argument and the rest with
# -object. The arguments after the binary are the sources to report on.
list(POP_FRONT BINARIES first_binary)
list(TRANSFORM BINARIES PREPEND "-object=")
set(cov_args
  "-instr-profile=${profdata}"
  "${first_binary}"
  ${BINARIES}
  "${SOURCE_DIR}")

execute_process(
  COMMAND "${LLVM_COV}" report ${cov_args}
  OUTPUT_FILE "${COVERAGE_DIR}/report.txt"
  COMMAND_ERROR_IS_FATAL ANY)
execute_process(
  COMMAND "${LLVM_COV}" show ${cov_args}
  OUTPUT_FILE "${COVERAGE_DIR}/lines.txt"
  COMMAND_ERROR_IS_FATAL ANY)
