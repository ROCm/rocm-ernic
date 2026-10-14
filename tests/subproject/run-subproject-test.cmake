# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Build rocm-ernic as a subproject of the parent project in parent/
#
# Usage:
#   cmake -DSETTINGS=<settings.cmake> [-DCONFIG=<config>]
#     -P run-subproject-test.cmake
#
# SETTINGS is written by CMakeLists.txt in this directory. CONFIG is the
# build configuration to build, for multi-config generators.
#
# Steps:
#   1. Configure the parent project, which checks that rocm-ernic leaves
#      the parent's build type and install prefix alone
#   2. Build the rocm-ernic server in it

cmake_minimum_required(VERSION 3.21)

if(NOT SETTINGS)
    message(FATAL_ERROR "run-subproject-test: SETTINGS is not set")
endif()
include(${SETTINGS})

set(parent_build_dir ${TEST_DIR}/parent-build)

# Run a command and fail with its output if it fails
function(run_step description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
          "run-subproject-test: ${description} failed:\n${output}")
    endif()
    message(STATUS "run-subproject-test: ${description}: OK")
endfunction()

file(REMOVE_RECURSE ${TEST_DIR})
file(MAKE_DIRECTORY ${TEST_DIR})

set(generator_args -G ${GENERATOR})
if(MAKE_PROGRAM)
    list(APPEND generator_args -DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM})
endif()

run_step("configure the parent project"
    ${CMAKE_COMMAND} -S ${PARENT_SOURCE_DIR} -B ${parent_build_dir}
        ${generator_args} -C ${PARENT_CACHE})

set(config_args)
if(CONFIG)
    set(config_args --config ${CONFIG})
endif()
run_step("build rocm-ernic in the parent project"
    ${CMAKE_COMMAND} --build ${parent_build_dir} --target rocm-ernic
        ${config_args})
