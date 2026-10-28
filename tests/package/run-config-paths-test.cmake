# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Build rocm-ernic's DEB packages with prefixes that move the
# configuration, and check where it goes and where the installed files
# read it
#
# Usage:
#   cmake -DSETTINGS=<settings.cmake> [-DCONFIG=<config>]
#     -P run-config-paths-test.cmake
#
# SETTINGS is written by CMakeLists.txt in this directory. CONFIG is the
# build configuration to build, for multi-config generators.
#
# Each case configures rocm-ernic in a build directory of its own, with
# the service files, builds the server, and runs run-package-test.cmake
# on it. It then checks the configuration file's path in the packages,
# and the path the service unit and ernicctl read it from, against
# literal values: run-package-test.cmake works them out the way the
# install rules do, so it can't tell if both are wrong. Other cases
# configure with install paths the installed files can't hold, and check
# that they're refused.

cmake_minimum_required(VERSION 3.21)

if(NOT SETTINGS)
    message(FATAL_ERROR "run-config-paths-test: SETTINGS is not set")
endif()
include(${SETTINGS})

# Fail the test with a message
function(fail)
    string(JOIN "" text ${ARGN})
    message(FATAL_ERROR "run-config-paths-test: ${text}")
endfunction()

# Run a command and fail with its output if it fails
function(run_step description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        fail("${description} failed:\n${output}")
    endif()
endfunction()

# Fail unless a file in the extracted packages has a line that is
# exactly the expected one
function(check_line root file expected)
    if(NOT EXISTS "${root}${file}")
        fail("the packages don't contain ${file}")
    endif()
    file(STRINGS "${root}${file}" lines)
    if(NOT expected IN_LIST lines)
        fail("${file} has no line '${expected}'")
    endif()
endfunction()

# Set env_var and dir_var to the ENV_FILE and TEST_DIR of a case's
# package test settings. They're included in this function so that they
# don't replace this test's own settings.
function(read_package_settings file env_var dir_var)
    include(${file})
    set(${env_var} "${ENV_FILE}" PARENT_SCOPE)
    set(${dir_var} "${TEST_DIR}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE ${TEST_DIR})
file(MAKE_DIRECTORY ${TEST_DIR})

set(generator_args -G ${GENERATOR})
if(MAKE_PROGRAM)
    list(APPEND generator_args -DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM})
endif()
set(config_args)
if(CONFIG)
    set(config_args --config ${CONFIG})
endif()

# Configure, build and package one case, and check its paths
#
#   name      The case's build directory, under TEST_DIR
#   prefix    CMAKE_INSTALL_PREFIX
#   env_file  Where the packages should install rocm-ernic.env
#   read      Where the installed files should read it from
#   ARGN      More options for configuring
function(check_case name prefix env_file read)
    set(build_dir ${TEST_DIR}/${name})
    run_step("${name}: configure"
        ${CMAKE_COMMAND} -S ${SOURCE_DIR} -B ${build_dir}
            ${generator_args} -C ${INITIAL_CACHE}
            -DCMAKE_INSTALL_PREFIX=${prefix}
            -DERNIC_INSTALL_SERVICE=ON
            ${ARGN})
    run_step("${name}: build the server"
        ${CMAKE_COMMAND} --build ${build_dir} --target rocm-ernic
            ${config_args})

    set(package_settings
        ${build_dir}/tests/package/package-test-settings.cmake)
    if(NOT EXISTS ${package_settings})
        fail("${name}: the package test isn't set up in ${build_dir}")
    endif()
    read_package_settings(${package_settings} installed_env_file
        package_test_dir)
    if(NOT installed_env_file STREQUAL env_file)
        fail("${name}: rocm-ernic.env is installed as ${installed_env_file}, "
            "not ${env_file}")
    endif()

    set(package_config_args)
    if(CONFIG)
        set(package_config_args -DCONFIG=${CONFIG})
    endif()
    run_step("${name}: run-package-test.cmake"
        ${CMAKE_COMMAND} -DSETTINGS=${package_settings} ${package_config_args}
            -P ${PACKAGE_TEST})

    # run-package-test.cmake extracts the packages here
    set(root ${package_test_dir}/root)
    if(NOT EXISTS ${root}${env_file})
        fail("${name}: the packages don't contain ${env_file}")
    endif()
    set(unit ${prefix}/lib/systemd/system/rocm-ernic.service)
    check_line(${root} ${unit} "EnvironmentFile=${read}")
    check_line(${root} ${unit}
        "ExecStart=${prefix}/libexec/rocm-ernic/rocm-ernic-launcher")
    check_line(${root} ${prefix}/bin/ernicctl "ENV_FILE = \"${read}\"")

    message(STATUS "run-config-paths-test: ${name}: OK")
endfunction()

# Configure one case that has to be refused, and check the error
#
#   name     The case's build directory, under TEST_DIR
#   prefix   CMAKE_INSTALL_PREFIX
#   message  A regular expression the error has to match, with its
#            whitespace as single spaces
#   ARGN     More options for configuring
function(check_refused name prefix message)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -S ${SOURCE_DIR} -B ${TEST_DIR}/${name}
            ${generator_args} -C ${INITIAL_CACHE}
            -DCMAKE_INSTALL_PREFIX=${prefix}
            -DERNIC_INSTALL_SERVICE=ON
            ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(result EQUAL 0)
        fail("${name}: configuring succeeded, but should have failed")
    endif()
    # CMake wraps the error's lines, so match it as one line
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${output}")
    if(NOT flat MATCHES "${message}")
        fail("${name}: configuring failed, but not with '${message}':\n"
            "${output}")
    endif()
    message(STATUS "run-config-paths-test: ${name}: OK")
endfunction()

# GNUInstallDirs puts the configuration of a package in /opt/<name> in
# /etc/opt/<name>, outside the prefix, where it's read
check_case(opt /opt/ernic-test
    /etc/opt/ernic-test/rocm-ernic/rocm-ernic.env
    /etc/opt/ernic-test/rocm-ernic/rocm-ernic.env)

# An absolute CMAKE_INSTALL_SYSCONFDIR under the prefix is the same as
# the default etc: the installed file is a copy to put in /etc
check_case(sysconf-in-prefix /usr/local
    /usr/local/etc/rocm-ernic/rocm-ernic.env
    /etc/rocm-ernic/rocm-ernic.env
    -DCMAKE_INSTALL_SYSCONFDIR=/usr/local/etc)

# An absolute CMAKE_INSTALL_SYSCONFDIR outside the prefix is read where
# it is, even under /usr/local. The prefix starts with /etc/rocm-ernic/,
# the path the source files read the configuration from, which mustn't
# change the installed programs' paths.
check_case(sysconf-outside-prefix /etc/rocm-ernic/app
    /usr/local/etc/rocm-ernic/rocm-ernic.env
    /usr/local/etc/rocm-ernic/rocm-ernic.env
    -DCMAKE_INSTALL_SYSCONFDIR=/usr/local/etc)

# Paths that can't go into the installed scripts and units as they are
check_refused(quote-in-sysconfdir /usr
    "writes ERNIC_ENV_FILE \\(/etc/site\"west/[^)]*\\) into scripts"
    "-DCMAKE_INSTALL_SYSCONFDIR=/etc/site\"west")
check_refused(dollar-in-prefix "/opt/ernic$HOME"
    "writes CMAKE_INSTALL_FULL_BINDIR \\(/opt/ernic\\$HOME/bin\\)")
check_refused(paren-in-prefix "/opt/ernic(test)"
    "writes CMAKE_INSTALL_FULL_BINDIR \\(/opt/ernic\\(test\\)/bin\\)")

message(STATUS "run-config-paths-test: OK")
