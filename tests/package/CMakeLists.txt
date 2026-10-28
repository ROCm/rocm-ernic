# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Build the DEB packages and check them (see run-package-test.cmake).
# This catches problems with the install rules and the packaging, which
# no other test uses, on every change rather than when the nightly
# packages are next built.
#
# Only a top-level build has packages (see ErnicPackage.cmake). The test
# needs dpkg's tools to build and read them, and file, which CPack uses
# to pick the files to give dpkg-shlibdeps. Without them the test isn't
# registered, rather than registered and skipped, since a skipped test
# here means something is wrong.
if(NOT PROJECT_IS_TOP_LEVEL OR CMAKE_SKIP_INSTALL_RULES)
    return()
endif()

# Cached, so the settings, written from the top-level directory, can
# read them
find_program(ERNIC_DPKG dpkg)
find_program(ERNIC_DPKG_DEB dpkg-deb)
find_program(ERNIC_DPKG_QUERY dpkg-query)
find_program(ERNIC_DPKG_SHLIBDEPS dpkg-shlibdeps)
find_program(ERNIC_FILE file)
mark_as_advanced(ERNIC_DPKG ERNIC_DPKG_DEB ERNIC_DPKG_QUERY
    ERNIC_DPKG_SHLIBDEPS ERNIC_FILE)
foreach(tool IN ITEMS
        ERNIC_DPKG
        ERNIC_DPKG_DEB
        ERNIC_DPKG_QUERY
        ERNIC_DPKG_SHLIBDEPS
        ERNIC_FILE
        CMAKE_READELF)
    if(NOT ${tool})
        message(STATUS
          "Not testing the packages: dpkg, dpkg-deb, dpkg-query, "
          "dpkg-shlibdeps (from dpkg-dev), file, or readelf is missing")
        return()
    endif()
endforeach()

set(ERNIC_PACKAGE_TEST_SETTINGS
    ${CMAKE_CURRENT_BINARY_DIR}/package-test-settings.cmake)

# The install rules and the packages are set up after this directory
# (see the top-level CMakeLists.txt), so the settings are written once
# the whole project is configured
function(ernic_write_package_test_settings file test_dir)
    file(CONFIGURE
        OUTPUT ${file}
        CONTENT [[
set(TEST_DIR "@test_dir@")
set(SOURCE_DIR "@PROJECT_SOURCE_DIR@")
set(CPACK_CONFIG "@PROJECT_BINARY_DIR@/CPackConfig.cmake")
set(VERSION "@PROJECT_VERSION@-@ERNIC_PACKAGE_RELEASE@")
set(INSTALL_PREFIX "@CMAKE_INSTALL_PREFIX@")
set(BINDIR "@CMAKE_INSTALL_FULL_BINDIR@")
set(LIBDIR "@CMAKE_INSTALL_FULL_LIBDIR@")
set(LIBEXECDIR "@CMAKE_INSTALL_FULL_LIBEXECDIR@")
set(DATADIR "@CMAKE_INSTALL_FULL_DATADIR@")
set(DOCDIR "@CMAKE_INSTALL_FULL_DOCDIR@")
set(ENV_FILE "@ERNIC_ENV_FILE@")
set(INSTALL_SERVICE "@ERNIC_INSTALL_SERVICE@")
set(BUNDLE_VFIO_USER "@ERNIC_BUNDLE_VFIO_USER@")
set(VFIO_USER_SONAME "@ernic_vfio_user_soname@")
set(VFIO_USER_LICENSE "@ERNIC_VFIO_USER_LICENSE@")
set(DPKG "@ERNIC_DPKG@")
set(DPKG_DEB "@ERNIC_DPKG_DEB@")
set(DPKG_QUERY "@ERNIC_DPKG_QUERY@")
set(READELF "@CMAKE_READELF@")
]]
        @ONLY)
endfunction()

# A deferred call's arguments are evaluated when it runs, in the
# top-level directory, so use EVAL to pass this directory's values now
cmake_language(EVAL CODE "
  cmake_language(DEFER DIRECTORY [[${PROJECT_SOURCE_DIR}]]
    CALL ernic_write_package_test_settings
      [[${ERNIC_PACKAGE_TEST_SETTINGS}]]
      [[${CMAKE_CURRENT_BINARY_DIR}/package-test]])")

add_test(
    NAME ernic-package-deb
    COMMAND ${CMAKE_COMMAND}
        -DSETTINGS=${ERNIC_PACKAGE_TEST_SETTINGS}
        -DCONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run-package-test.cmake
)
set_tests_properties(ernic-package-deb PROPERTIES TIMEOUT 300)

# Build the packages again with prefixes that move the configuration,
# such as /opt/<name> (see run-config-paths-test.cmake). Each case
# configures and builds the server in a build directory of its own,
# with this build's compiler and dependencies, as cmake-subproject
# does. It doesn't run the server, so it's built without sanitizers.
set(ERNIC_CONFIG_PATHS_CACHE
    ${CMAKE_CURRENT_BINARY_DIR}/config-paths-cache.cmake)
set(ERNIC_CONFIG_PATHS_CACHE_ENTRIES)
foreach(var IN ITEMS CMAKE_PREFIX_PATH CMAKE_C_COMPILER ERNIC_WERROR)
    string(APPEND ERNIC_CONFIG_PATHS_CACHE_ENTRIES
        "set(${var} \"${${var}}\" CACHE STRING \"\")\n")
endforeach()
file(WRITE ${ERNIC_CONFIG_PATHS_CACHE}
    "${ERNIC_CONFIG_PATHS_CACHE_ENTRIES}")

set(ERNIC_CONFIG_PATHS_TEST_SETTINGS
    ${CMAKE_CURRENT_BINARY_DIR}/config-paths-test-settings.cmake)
file(CONFIGURE
    OUTPUT ${ERNIC_CONFIG_PATHS_TEST_SETTINGS}
    CONTENT [[
set(TEST_DIR "@CMAKE_CURRENT_BINARY_DIR@/config-paths-test")
set(SOURCE_DIR "@PROJECT_SOURCE_DIR@")
set(INITIAL_CACHE "@ERNIC_CONFIG_PATHS_CACHE@")
set(GENERATOR "@CMAKE_GENERATOR@")
set(MAKE_PROGRAM "@CMAKE_MAKE_PROGRAM@")
set(PACKAGE_TEST "@CMAKE_CURRENT_SOURCE_DIR@/run-package-test.cmake")
]]
    @ONLY)

add_test(
    NAME ernic-package-deb-config-paths
    COMMAND ${CMAKE_COMMAND}
        -DSETTINGS=${ERNIC_CONFIG_PATHS_TEST_SETTINGS}
        -DCONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run-config-paths-test.cmake
)
set_tests_properties(ernic-package-deb-config-paths PROPERTIES TIMEOUT 600)
