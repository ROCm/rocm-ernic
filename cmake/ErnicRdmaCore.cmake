# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# ErnicRdmaCore.cmake
#
# Builds rdma-core with the rocm_ernic provider.
# Downloads the upstream tarball, injects the
# provider, and builds/installs via a helper
# script.
#
# Included from the top-level CMakeLists.txt.

set(RDMA_CORE_VERSION "64.0" CACHE STRING
    "Upstream rdma-core version to download")

option(ERNIC_RDMA_CORE_BUILD
    "Build rdma-core with rocm_ernic provider" OFF)

# -----------------------------------------------
# Derived paths
# -----------------------------------------------

set(RDMA_CORE_DEPS_DIR
    "${CMAKE_BINARY_DIR}/_deps/rdma-core")
set(RDMA_CORE_INSTALL_DIR
    "${RDMA_CORE_DEPS_DIR}/install")

set(RDMA_CORE_BUILD_SCRIPT
    "${CMAKE_SOURCE_DIR}/scripts/build-rdma-core.sh")

# -----------------------------------------------
# Build targets
# -----------------------------------------------

if(ERNIC_RDMA_CORE_BUILD)
    include(ProcessorCount)
    ProcessorCount(NPROC)
    if(NPROC EQUAL 0)
        set(NPROC 4)
    endif()

    set(ERNIC_PROVIDER_DIR
        "${CMAKE_SOURCE_DIR}/rdma-core/providers/rocm_ernic")
    if(NOT IS_DIRECTORY "${ERNIC_PROVIDER_DIR}"
       OR NOT EXISTS
       "${ERNIC_PROVIDER_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "ERNIC_RDMA_CORE_BUILD=ON but provider"
            " source not found at"
            " ${ERNIC_PROVIDER_DIR}")
    endif()

    set(ERNIC_RDMA_CORE_ENV
        ${CMAKE_COMMAND} -E env
            "RDMA_CORE_VERSION=${RDMA_CORE_VERSION}"
    )

    message(STATUS
        "rdma-core: build v${RDMA_CORE_VERSION}"
        " with rocm_ernic provider")

    add_custom_target(build-rdma-core
        COMMAND ${ERNIC_RDMA_CORE_ENV}
            ${RDMA_CORE_BUILD_SCRIPT}
            ${RDMA_CORE_DEPS_DIR}
            ${RDMA_CORE_INSTALL_DIR}
            ${NPROC}
            --build-only
        COMMENT
            "Building rdma-core with rocm_ernic"
            " provider (no install)"
        VERBATIM
    )

    add_custom_target(install-rdma-core
        COMMAND ${ERNIC_RDMA_CORE_ENV}
            ${RDMA_CORE_BUILD_SCRIPT}
            ${RDMA_CORE_DEPS_DIR}
            ${RDMA_CORE_INSTALL_DIR}
            ${NPROC}
        COMMENT
            "Building and installing rdma-core"
            " with rocm_ernic provider"
        VERBATIM
    )

    set(RDMA_CORE_LIB_DIR
        "${RDMA_CORE_INSTALL_DIR}/lib"
        CACHE PATH
        "rdma-core library directory" FORCE)
    set(RDMA_CORE_INCLUDE_DIR
        "${RDMA_CORE_INSTALL_DIR}/include"
        CACHE PATH
        "rdma-core include directory" FORCE)

    message(STATUS
        "rdma-core: will install to"
        " ${RDMA_CORE_INSTALL_DIR}")
endif()
