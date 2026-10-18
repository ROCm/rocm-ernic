# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Build rocm-ernic as a subproject of another CMake project (see
# run-subproject-test.cmake). This catches CMake code that assumes
# rocm-ernic is the top-level project, which nothing else here does.

# The parent project builds rocm-ernic with this build's compiler,
# options, and dependencies. It doesn't build the tests, which would make
# the test take much longer. PKG_CONFIG_PATH, if set, reaches the parent
# through the environment. CMAKE_PREFIX_PATH is read as a normal
# variable, not from the cache, so that it includes any prefixes a
# project above rocm-ernic added with list(APPEND).
set(ERNIC_SUBPROJECT_CACHE ${CMAKE_CURRENT_BINARY_DIR}/parent-cache.cmake)
string(CONCAT ERNIC_SUBPROJECT_CACHE_ENTRIES
    "set(ERNIC_SUBPROJECT_SOURCE_DIR \"${PROJECT_SOURCE_DIR}\" CACHE PATH \"\")\n"
    "set(BUILD_TESTING OFF CACHE BOOL \"\")\n")
foreach(var IN ITEMS
        CMAKE_PREFIX_PATH
        CMAKE_C_COMPILER
        ERNIC_WERROR
        ERNIC_USE_SANITIZERS
        ERNIC_USE_THREAD_SANITIZER
        ERNIC_SANITIZER_SHARED_RUNTIME)
    string(APPEND ERNIC_SUBPROJECT_CACHE_ENTRIES
        "set(${var} \"${${var}}\" CACHE STRING \"\")\n")
endforeach()
file(WRITE ${ERNIC_SUBPROJECT_CACHE} "${ERNIC_SUBPROJECT_CACHE_ENTRIES}")

set(ERNIC_SUBPROJECT_TEST_SETTINGS
    ${CMAKE_CURRENT_BINARY_DIR}/subproject-test-settings.cmake)
file(CONFIGURE
    OUTPUT ${ERNIC_SUBPROJECT_TEST_SETTINGS}
    CONTENT [[
set(TEST_DIR "@CMAKE_CURRENT_BINARY_DIR@/subproject-test")
set(PARENT_SOURCE_DIR "@CMAKE_CURRENT_SOURCE_DIR@/parent")
set(PARENT_CACHE "@ERNIC_SUBPROJECT_CACHE@")
set(GENERATOR "@CMAKE_GENERATOR@")
set(MAKE_PROGRAM "@CMAKE_MAKE_PROGRAM@")
]]
    @ONLY)

add_test(
    NAME cmake-subproject
    COMMAND ${CMAKE_COMMAND}
        -DSETTINGS=${ERNIC_SUBPROJECT_TEST_SETTINGS}
        -DCONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run-subproject-test.cmake
)
# Configures and builds the server from scratch, so give it time under
# the sanitizers
set_tests_properties(cmake-subproject PROPERTIES TIMEOUT 600)
