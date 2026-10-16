# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Optional clang-tidy integration
#
# With ERNIC_USE_CLANG_TIDY on, every target that goes through
# ernic_set_compiler_flags() runs clang-tidy on its sources as they
# compile, with the checks in .clang-tidy. Every finding is an error, so
# it fails the build.
#
# Follows the pattern from hipObject's HIPOBJClangTidy.cmake.

include_guard(GLOBAL)

option(ERNIC_USE_CLANG_TIDY
  "Run clang-tidy when compiling" OFF)

if(ERNIC_USE_CLANG_TIDY)
    # The vendored sources are compiled in the same targets as ours, and
    # the SKIP_LINTING source property, which keeps clang-tidy off them,
    # is new in CMake 3.27
    if(CMAKE_VERSION VERSION_LESS 3.27)
        message(FATAL_ERROR
          "ERNIC_USE_CLANG_TIDY needs CMake 3.27 or later")
    endif()

    # clang-tidy parses each file with the compiler's flags, and rejects
    # the GCC-only ones
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR
          "ERNIC_USE_CLANG_TIDY requires Clang as the C compiler "
          "(CMAKE_C_COMPILER)")
    endif()

    find_program(ERNIC_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)

    # .clang-tidy uses ExcludeHeaderFilterRegex, which is new in
    # clang-tidy 19. An older one rejects the whole file.
    execute_process(
      COMMAND "${ERNIC_CLANG_TIDY_EXE}" --version
      OUTPUT_VARIABLE ernic_clang_tidy_version_output
      RESULT_VARIABLE ernic_clang_tidy_version_result)
    if(NOT ernic_clang_tidy_version_result EQUAL 0
        OR NOT ernic_clang_tidy_version_output
          MATCHES "version ([0-9]+)\\.")
        message(FATAL_ERROR
          "Can't get the version of ${ERNIC_CLANG_TIDY_EXE}")
    endif()
    if(CMAKE_MATCH_1 VERSION_LESS 19)
        message(FATAL_ERROR
          "ERNIC_USE_CLANG_TIDY needs clang-tidy 19 or later, but "
          "${ERNIC_CLANG_TIDY_EXE} is version ${CMAKE_MATCH_1}")
    endif()

    message(STATUS "clang-tidy enabled: ${ERNIC_CLANG_TIDY_EXE}")

    # clang-tidy doesn't know every warning flag the build uses when it's
    # a different version from the compiler, so don't warn about those
    set(ERNIC_CLANG_TIDY_COMMAND
      "${ERNIC_CLANG_TIDY_EXE}" "--extra-arg=-Wno-unknown-warning-option")
endif()

# Run clang-tidy on a target's sources
#
# This is set per target instead of with CMAKE_C_CLANG_TIDY so that a
# superproject's targets aren't checked. ernic_set_compiler_flags() keeps
# it off the vendored sources under third-party/.
function(ernic_set_clang_tidy target)
    if(NOT ERNIC_USE_CLANG_TIDY)
        return()
    endif()

    set_target_properties(${target} PROPERTIES
      C_CLANG_TIDY "${ERNIC_CLANG_TIDY_COMMAND}")
endfunction()
