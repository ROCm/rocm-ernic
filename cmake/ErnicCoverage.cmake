# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Code coverage (LLVM source-based coverage, Clang only)
#
# With ERNIC_USE_CODE_COVERAGE on, every target that goes through
# ernic_set_compiler_flags() is instrumented: the server and the test
# programs. Each instrumented program writes its profile to
# coverage/profraw in the build directory, however it's run, so the
# servers the CI scripts start count too. The ernic-coverage target
# reports the coverage of the project's own sources (src/) from them:
#
#   ctest
#   cmake --build . --target ernic-coverage
#
# The report goes in coverage/report.txt (a summary for each file) and
# coverage/lines.txt (the coverage of each line). The profiles add up
# over test runs; delete coverage/profraw to start over.

include_guard(GLOBAL)

option(ERNIC_USE_CODE_COVERAGE
  "Build with LLVM code coverage instrumentation (Clang only)" OFF)

set(ERNIC_COVERAGE_DIR "${PROJECT_BINARY_DIR}/coverage")

# Find the llvm-profdata and llvm-cov that match the compiler, since the
# profile format changes between LLVM versions. Look next to the compiler
# first, and for the versioned names that Linux distributions use.
# CMAKE_PREFIX_PATH, as a CMake or environment variable, is skipped
# because CMake searches it before HINTS, and an LLVM installed under
# one of its prefixes may not be the compiler's.
function(ernic_find_coverage_tools)
    string(REGEX MATCH "^[0-9]+" major "${CMAKE_C_COMPILER_VERSION}")
    cmake_path(GET CMAKE_C_COMPILER PARENT_PATH compiler_dir)
    file(REAL_PATH "${CMAKE_C_COMPILER}" compiler_path)
    cmake_path(GET compiler_path PARENT_PATH compiler_real_dir)

    foreach(tool IN ITEMS profdata cov)
        string(TOUPPER ${tool} tool_upper)
        find_program(ERNIC_LLVM_${tool_upper}
          NAMES llvm-${tool}-${major} llvm-${tool}
          NAMES_PER_DIR
          HINTS "${compiler_dir}" "${compiler_real_dir}"
          NO_CMAKE_PATH
          NO_CMAKE_ENVIRONMENT_PATH
          DOC "llvm-${tool} for the code coverage report")
        if(NOT ERNIC_LLVM_${tool_upper})
            message(FATAL_ERROR
              "ERNIC_USE_CODE_COVERAGE needs llvm-${tool}, which wasn't "
              "found with ${CMAKE_C_COMPILER}. Set ERNIC_LLVM_${tool_upper} "
              "to its path.")
        endif()
    endforeach()
endfunction()

# Add the code coverage instrumentation to a target
#
# The profile's file name is built in, so programs that the tests run
# indirectly, such as the server under the CI scripts, record their
# coverage too. %m gives each executable a file of its own, which the
# runtime merges each run into, so concurrent runs don't collide.
# LLVM_PROFILE_FILE overrides the name.
function(ernic_add_coverage target)
    target_compile_options(${target} PRIVATE
      "-fprofile-instr-generate=${ERNIC_COVERAGE_DIR}/profraw/%m.profraw"
      -fcoverage-mapping)
    target_link_options(${target} PRIVATE -fprofile-instr-generate)

    # The report reads the coverage mapping from the linked binaries
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE" OR type STREQUAL "SHARED_LIBRARY")
        set_property(GLOBAL APPEND PROPERTY
          ERNIC_COVERAGE_BINARIES "$<TARGET_FILE:${target}>")
    endif()
endfunction()

# Add the ernic-coverage target, which merges the profiles and writes
# the report (see ErnicCoverageReport.cmake). It has to run once all
# the instrumented targets have been added. The binaries' generator
# expressions make the target depend on them, so it builds any that are
# out of date.
function(ernic_add_coverage_target)
    get_property(binaries GLOBAL PROPERTY ERNIC_COVERAGE_BINARIES)
    add_custom_target(ernic-coverage
      COMMAND ${CMAKE_COMMAND}
        "-DLLVM_PROFDATA=${ERNIC_LLVM_PROFDATA}"
        "-DLLVM_COV=${ERNIC_LLVM_COV}"
        "-DCOVERAGE_DIR=${ERNIC_COVERAGE_DIR}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}/src"
        "-DBINARIES=${binaries}"
        -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ErnicCoverageReport.cmake"
      COMMAND ${CMAKE_COMMAND} -E cat "${ERNIC_COVERAGE_DIR}/report.txt"
      COMMENT "Generating the code coverage report"
      VERBATIM)
endfunction()

if(ERNIC_USE_CODE_COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR
          "ERNIC_USE_CODE_COVERAGE requires Clang as the C compiler "
          "(CMAKE_C_COMPILER)")
    endif()
    ernic_find_coverage_tools()
    cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}"
      CALL ernic_add_coverage_target)
endif()
