# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Optional include-what-you-use (IWYU) integration
#
# With ERNIC_USE_IWYU on, every target that goes through
# ernic_set_compiler_flags() runs IWYU on its sources as they compile,
# which prints suggestions for #include lines to add or remove. They are
# advisory: IWYU doesn't fail the build.
#
# Follows the pattern from hipObject's HIPOBJIWYU.cmake.

include_guard(GLOBAL)

option(ERNIC_USE_IWYU
  "Run include-what-you-use when compiling" OFF)

if(ERNIC_USE_IWYU)
    # The vendored sources are compiled in the same targets as ours, and
    # the SKIP_LINTING source property, which keeps IWYU off them, is new
    # in CMake 3.27
    if(CMAKE_VERSION VERSION_LESS 3.27)
        message(FATAL_ERROR
          "ERNIC_USE_IWYU needs CMake 3.27 or later")
    endif()

    # IWYU parses with its own clang, which rejects the options that it
    # doesn't know, such as -fzero-init-padding-bits, which the build
    # uses with GCC 15 and later
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        message(WARNING
          "IWYU's clang may not accept the options that the build uses "
          "with GCC. Configure ERNIC_USE_IWYU builds with the clang that "
          "IWYU is built on (include-what-you-use --version).")
    endif()

    find_program(ERNIC_IWYU_EXE NAMES include-what-you-use REQUIRED)
    message(STATUS "include-what-you-use enabled: ${ERNIC_IWYU_EXE}")

    # iwyu-libc.imp: glibc declares some symbols in private bits/*
    # headers that IWYU would otherwise suggest including directly, and
    # POSIX lets several headers define types such as ssize_t, where the
    # bundled IWYU mappings only accept one. Redirect them to the public
    # headers.
    #
    # iwyu-ernic.imp: the QEMU compatibility headers repeat typedefs such
    # as PCIDevice and dma_addr_t (which C11 allows), and IWYU would
    # otherwise insist on whichever one it saw last. Accept any of them.
    set(ERNIC_IWYU_MAPPINGS
      "${CMAKE_CURRENT_LIST_DIR}/iwyu-ernic.imp"
      "${CMAKE_CURRENT_LIST_DIR}/iwyu-libc.imp")

    # IWYU parses with its own clang, which doesn't know every warning
    # flag the build uses (e.g., the GCC-only ones), so don't warn about
    # those
    set(ERNIC_IWYU_COMMAND "${ERNIC_IWYU_EXE}" "-Wno-unknown-warning-option")
    foreach(mapping IN LISTS ERNIC_IWYU_MAPPINGS)
        if(NOT EXISTS "${mapping}")
            message(FATAL_ERROR "IWYU mapping file not found: ${mapping}")
        endif()
        list(APPEND ERNIC_IWYU_COMMAND "-Xiwyu" "--mapping_file=${mapping}")
    endforeach()
endif()

# Run include-what-you-use on a target's sources
#
# This is set per target instead of with CMAKE_C_INCLUDE_WHAT_YOU_USE so
# that a superproject's targets aren't checked. ernic_set_compiler_flags()
# keeps it off the vendored sources under third-party/.
function(ernic_set_iwyu target)
    if(NOT ERNIC_USE_IWYU)
        return()
    endif()

    set_target_properties(${target} PROPERTIES
      C_INCLUDE_WHAT_YOU_USE "${ERNIC_IWYU_COMMAND}")
endfunction()
