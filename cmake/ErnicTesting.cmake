# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Helpers for building the test programs and registering them with CTest

include_guard(GLOBAL)

# ernic_add_test_program(<name> SOURCES <file>...
#                        [DEPS <dep>...] [HEADER_DEPS <dep>...])
#
# Build the test program <name> from SOURCES with the project's warning,
# hardening, sanitizer and analysis settings (ernic_set_compiler_flags()),
# and the project's include roots: src/, src/qemu-compat/include/ and
# shared/.
#
# DEPS names the libraries from outside the project that the program
# links, and HEADER_DEPS those whose headers it only includes, such as
# one whose functions the test stubs. Their headers are SYSTEM includes,
# as for the server, so their warnings stay quiet while the sources under
# test are still warning-checked. A dependency is one of:
#
#   glib       GLib
#   ibverbs    libibverbs
#   vfio-user  libvfio-user
#   json-c     json-c (DEPS only; exists only with ERNIC_BUILD_P4)
#   pthread    the thread library (DEPS only)
#
# Add anything else the program needs, such as compile definitions or
# link options, to the target afterwards.
function(ernic_add_test_program name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEPS;HEADER_DEPS")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "ernic_add_test_program(${name}): unknown "
          "arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_SOURCES)
        message(FATAL_ERROR "ernic_add_test_program(${name}): no SOURCES")
    endif()

    add_executable(${name} ${arg_SOURCES})
    target_include_directories(${name} PRIVATE
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/qemu-compat/include
      ${PROJECT_SOURCE_DIR}/shared
    )

    foreach(kind IN ITEMS DEPS HEADER_DEPS)
        foreach(dep IN LISTS arg_${kind})
            unset(include_dirs)
            unset(link_dirs)
            unset(libraries)
            if(dep STREQUAL "glib")
                set(include_dirs ${GLIB2_INCLUDE_DIRS})
                set(link_dirs ${GLIB2_LIBRARY_DIRS})
                set(libraries ${GLIB2_LIBRARIES})
            elseif(dep STREQUAL "ibverbs")
                set(include_dirs ${ERNIC_IBVERBS_INCLUDE_DIRS})
                set(link_dirs ${IBVERBS_LIBRARY_DIRS})
                set(libraries ${IBVERBS_LIBRARIES})
            elseif(dep STREQUAL "vfio-user")
                set(include_dirs ${VFIO_USER_INCLUDE_DIRS})
                set(link_dirs ${VFIO_USER_LIBRARY_DIRS})
                set(libraries ${VFIO_USER_LINK_LIBRARIES})
            elseif(kind STREQUAL "DEPS" AND dep STREQUAL "json-c")
                # An imported target, which brings its own headers
                set(libraries PkgConfig::JSONC)
            elseif(kind STREQUAL "DEPS" AND dep STREQUAL "pthread")
                set(libraries pthread)
            else()
                message(FATAL_ERROR "ernic_add_test_program(${name}): "
                  "unknown ${kind} entry: ${dep}")
            endif()

            if(include_dirs)
                target_include_directories(${name} SYSTEM PRIVATE
                  ${include_dirs})
            endif()
            if(kind STREQUAL "DEPS")
                if(link_dirs)
                    target_link_directories(${name} PRIVATE ${link_dirs})
                endif()
                target_link_libraries(${name} PRIVATE ${libraries})
            endif()
        endforeach()
    endforeach()

    ernic_set_compiler_flags(${name})
endfunction()

# ernic_add_unit_test(<test> <program> TIMEOUT <seconds> [RUN_SERIAL]
#                     [ARGS <arg>...])
#
# Register the test <test>, which runs the test program <program> (a
# target) with ARGS and fails if it takes longer than TIMEOUT seconds.
# RUN_SERIAL runs it with no other test alongside, for one that binds a
# fixed port, say.
function(ernic_add_unit_test name program)
    cmake_parse_arguments(PARSE_ARGV 2 arg "RUN_SERIAL" "TIMEOUT" "ARGS")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "ernic_add_unit_test(${name}): unknown "
          "arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_TIMEOUT)
        message(FATAL_ERROR "ernic_add_unit_test(${name}): no TIMEOUT")
    endif()

    add_test(
      NAME ${name}
      COMMAND $<TARGET_FILE:${program}> ${arg_ARGS}
    )
    set_tests_properties(${name} PROPERTIES TIMEOUT ${arg_TIMEOUT})
    if(arg_RUN_SERIAL)
        set_tests_properties(${name} PROPERTIES RUN_SERIAL TRUE)
    endif()
endfunction()
