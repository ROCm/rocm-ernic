# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# A parent project that adds rocm-ernic with add_subdirectory(), the way
# a superproject would. Configuring it fails if rocm-ernic changes the
# parent's build type or install prefix, and building it fails if
# rocm-ernic finds its files relative to the parent's tree instead of
# its own. Configured and built by ../run-subproject-test.cmake.

cmake_minimum_required(VERSION 3.21)

project(ernic_subproject_parent LANGUAGES C)

if(NOT ERNIC_SUBPROJECT_SOURCE_DIR)
    message(FATAL_ERROR "ERNIC_SUBPROJECT_SOURCE_DIR is not set")
endif()

set(parent_build_type "$CACHE{CMAKE_BUILD_TYPE}")
set(parent_install_prefix "$CACHE{CMAKE_INSTALL_PREFIX}")

add_subdirectory(${ERNIC_SUBPROJECT_SOURCE_DIR} rocm-ernic)

if(NOT "$CACHE{CMAKE_BUILD_TYPE}" STREQUAL "${parent_build_type}")
    message(FATAL_ERROR
      "rocm-ernic changed the parent's CMAKE_BUILD_TYPE from "
      "'${parent_build_type}' to '$CACHE{CMAKE_BUILD_TYPE}'")
endif()
if(NOT "$CACHE{CMAKE_INSTALL_PREFIX}" STREQUAL "${parent_install_prefix}")
    message(FATAL_ERROR
      "rocm-ernic changed the parent's CMAKE_INSTALL_PREFIX from "
      "'${parent_install_prefix}' to '$CACHE{CMAKE_INSTALL_PREFIX}'")
endif()
