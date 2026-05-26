# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Top-level CMake file for rocm-ernic

# CMake version according to latest ROCm platform requirements
cmake_minimum_required(VERSION 3.21)

# Set the default build type to Debug
#
# NOTE: This has to be done BEFORE the project is declared!
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type")
endif()

# Set the version in variables so we can use them in *.in files
# and avoid version mismatch.
set(ROCM_ERNIC_VERSION_MAJOR 0)
set(ROCM_ERNIC_VERSION_MINOR 2)
set(ROCM_ERNIC_VERSION_PATCH 0)
set(ROCM_ERNIC_VERSION
  "${ROCM_ERNIC_VERSION_MAJOR}.${ROCM_ERNIC_VERSION_MINOR}.${ROCM_ERNIC_VERSION_PATCH}"
)

project(rocm-ernic
    VERSION ${ROCM_ERNIC_VERSION}
    DESCRIPTION "Emulated RDMA NIC for Virtual Machines"
    LANGUAGES C
)

# Set the list of build types for ccmake/CMake GUI
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
  "Debug" "Release" "RelWithDebInfo" "MinSizeRel" "None"
)

# Add cmake/ to the module search path
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# Docs-only mode: build documentation without requiring
# libvfio-user, glib, or libibverbs.
option(ERNIC_DOCS_ONLY
  "Only configure documentation targets"
  OFF)

if(ERNIC_DOCS_ONLY)
    include(ErnicDocumentation)
    return()
endif()

# Enable CTest (automatically calls enable_testing())
include(CTest)

# ---------------------------------------------------------------
# Project-level modules
# ---------------------------------------------------------------
include(ErnicGitInfo)
include(ErnicCompilerOptions)

# ---------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------
find_package(PkgConfig REQUIRED)

# -- libvfio-user ------------------------------------------------
pkg_check_modules(VFIO_USER vfio-user)
if(NOT VFIO_USER_FOUND)
    # Variable names deliberately avoid the VFIO_USER_ prefix
    # because pkg_check_modules already created INTERNAL cache
    # entries (e.g. VFIO_USER_INCLUDEDIR) that would shadow
    # find_library / find_path results.
    find_library(VFIO_USER_FALLBACK_LIB
        NAMES vfio-user
        HINTS
            /usr/local/lib
            /usr/local/lib/x86_64-linux-gnu
            /usr/lib
            /usr/lib/x86_64-linux-gnu
        NO_DEFAULT_PATH
        DOC "Path to libvfio-user library"
    )
    find_path(VFIO_USER_FALLBACK_INC
        NAMES vfio-user/libvfio-user.h
        HINTS
            /usr/local/include
            /usr/include
        NO_DEFAULT_PATH
        DOC "Path to libvfio-user headers"
    )
    if(NOT VFIO_USER_FALLBACK_LIB OR
       NOT VFIO_USER_FALLBACK_INC)
        message(FATAL_ERROR
          "libvfio-user not found via pkg-config or in "
          "/usr/local.\n"
          "Install it from "
          "https://github.com/nutanix/libvfio-user\n"
          "or set CMAKE_PREFIX_PATH to its install location."
        )
    endif()
    set(VFIO_USER_INCLUDE_DIRS
      "${VFIO_USER_FALLBACK_INC}")
    set(VFIO_USER_LINK_LIBRARIES
      "${VFIO_USER_FALLBACK_LIB}")
    message(STATUS
      "Found libvfio-user (fallback): "
      "${VFIO_USER_FALLBACK_LIB}")
else()
    set(VFIO_USER_LINK_LIBRARIES "${VFIO_USER_LIBRARIES}")
    message(STATUS
      "Found libvfio-user via pkg-config: "
      "${VFIO_USER_VERSION}")
endif()

# -- glib-2.0 ----------------------------------------------------
pkg_check_modules(GLIB2 REQUIRED glib-2.0)
if(NOT GLIB2_FOUND)
    message(FATAL_ERROR
      "glib-2.0 not found. Install libglib2.0-dev "
      "(Ubuntu/Debian) or glib2-devel (Fedora/RHEL)."
    )
endif()

# -- libibverbs ---------------------------------------------------
pkg_check_modules(IBVERBS REQUIRED libibverbs)
if(NOT IBVERBS_FOUND)
    message(FATAL_ERROR
      "libibverbs not found. Install libibverbs-dev "
      "(Ubuntu/Debian) or libibverbs-devel (Fedora/RHEL)."
    )
endif()

# -- libfabric (optional; enables ofi-tcp mesh backend) ----------
pkg_check_modules(LIBFABRIC libfabric)
if(LIBFABRIC_FOUND)
  message(STATUS
    "Found libfabric via pkg-config: ${LIBFABRIC_VERSION} "
    "(ofi-tcp backend enabled)")
  set(ERNIC_HAVE_LIBFABRIC 1)
else()
  message(STATUS
    "libfabric not found — ofi-tcp backend will fail at runtime if selected; "
    "install libfabric-dev to enable.")
  set(ERNIC_HAVE_LIBFABRIC 0)
endif()

# ---------------------------------------------------------------
# Source file groups
# ---------------------------------------------------------------

# RDMA backend sources from QEMU port
# Note: rdma.c excluded -- only contains QEMU type registration
set(RDMA_BACKEND_SOURCES
    src/from-qemu/hw/rdma/rdma_backend.c
    src/from-qemu/hw/rdma/rdma_backend_core.c
    src/from-qemu/hw/rdma/rdma_backend_none.c
    src/from-qemu/hw/rdma/rdma_backend_loopback.c
    src/from-qemu/hw/rdma/ernic_ofi_wire.c
    src/from-qemu/hw/rdma/rdma_backend_tcp.c
    src/from-qemu/hw/rdma/rdma_utils.c
    src/from-qemu/hw/rdma/rdma_rm.c
)

# PVRDMA device sources from QEMU port
set(PVRDMA_DEVICE_SOURCES
    src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c
    src/from-qemu/hw/rdma/vmw/pvrdma_dev_ring.c
    src/from-qemu/hw/rdma/vmw/pvrdma_main.c
    src/from-qemu/hw/rdma/vmw/pvrdma_qp_ops.c
    src/from-qemu/hw/rdma/vmw/pvrdma_eth.c
)

# Utility sources
set(UTILITY_SOURCES
    src/from-qemu/utils/error-report.c
    src/from-qemu/utils/dhcp_server.c
    src/from-qemu/utils/dhcp_proxy.c
    src/from-qemu/utils/eth_rx_inject.c
    src/from-qemu/utils/tcp_conn.c
    src/from-qemu/utils/rdma_cm_proto.c
)

# QEMU stub implementations
set(QEMU_STUB_SOURCES
    src/from-qemu/qemu-stubs/qemu_stubs.c
    src/from-qemu/qemu-stubs/qemu_type_stubs.c
)

# Compatibility bridge layer
set(BRIDGE_SOURCES
    src/rocm_ernic_compat.c
)

# Main server implementation
set(SERVER_SOURCES
    src/rocm_ernic_server.c
)

set(ALL_SOURCES
    ${SERVER_SOURCES}
    ${BRIDGE_SOURCES}
    ${RDMA_BACKEND_SOURCES}
    ${PVRDMA_DEVICE_SOURCES}
    ${UTILITY_SOURCES}
    ${QEMU_STUB_SOURCES}
)

# ---------------------------------------------------------------
# Executable
# ---------------------------------------------------------------
add_executable(rocm-ernic ${ALL_SOURCES})

target_include_directories(rocm-ernic PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${IBVERBS_INCLUDE_DIRS}
)

if(LIBFABRIC_FOUND)
  target_include_directories(rocm-ernic PRIVATE ${LIBFABRIC_INCLUDE_DIRS})
endif()

target_compile_definitions(rocm-ernic PRIVATE
    GIT_SHA_FULL="${GIT_SHA_FULL}"
    GIT_SHA_SHORT="${GIT_SHA_SHORT}"
    ERNIC_HAVE_LIBFABRIC=${ERNIC_HAVE_LIBFABRIC}
    _GNU_SOURCE
    TARGET_PAGE_SIZE=4096
)

target_compile_options(rocm-ernic PRIVATE
    ${GLIB2_CFLAGS_OTHER}
    -g
)

# Suppress strict warnings for QEMU-ported upstream code.
# These files are derived from QEMU and will not be
# modified to fix sign-conversion, format, packed,
# switch-enum, etc. warnings.  Our own code (src/*.c,
# tests/) still gets full warnings via
# ernic_set_compiler_flags().
set(QEMU_PORTED_SOURCES
    ${RDMA_BACKEND_SOURCES}
    ${PVRDMA_DEVICE_SOURCES}
    ${UTILITY_SOURCES}
    ${QEMU_STUB_SOURCES}
)
set_source_files_properties(${QEMU_PORTED_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-w"
)

target_link_directories(rocm-ernic PRIVATE
    ${VFIO_USER_LIBRARY_DIRS}
    ${GLIB2_LIBRARY_DIRS}
    ${IBVERBS_LIBRARY_DIRS}
)

if(LIBFABRIC_FOUND)
  target_link_directories(rocm-ernic PRIVATE ${LIBFABRIC_LIBRARY_DIRS})
endif()

target_link_libraries(rocm-ernic PRIVATE
    ${VFIO_USER_LINK_LIBRARIES}
    ${GLIB2_LIBRARIES}
    ${IBVERBS_LIBRARIES}
)

if(LIBFABRIC_FOUND)
  target_link_libraries(rocm-ernic PRIVATE ${LIBFABRIC_LIBRARIES})
endif()

ernic_set_compiler_flags(rocm-ernic)

option(ERNIC_WERROR
  "Treat compiler warnings as errors for project code"
  ON)

if(ERNIC_WERROR)
    target_compile_options(rocm-ernic PRIVATE -Werror)
endif()

# ---------------------------------------------------------------
# Tests
# ---------------------------------------------------------------
add_subdirectory(tests)

# ---------------------------------------------------------------
# Install
# ---------------------------------------------------------------
include(ErnicInstall)

# ---------------------------------------------------------------
# Kernel module (DKMS)
# ---------------------------------------------------------------
include(ErnicKernelModule)

# ---------------------------------------------------------------
# rdma-core with rocm_ernic provider
# ---------------------------------------------------------------
include(ErnicRdmaCore)

# ---------------------------------------------------------------
# Documentation (optional, requires -DERNIC_BUILD_DOCS=ON)
# ---------------------------------------------------------------
include(ErnicDocumentation)

# ---------------------------------------------------------------
# Summary
# ---------------------------------------------------------------
message("")
message("=== rocm-ernic Configuration ===")
message("  Version      : ${PROJECT_VERSION}")
message("  Git SHA      : ${GIT_SHA_SHORT}")
message("  Build type   : ${CMAKE_BUILD_TYPE}")
message("  C compiler   : ${CMAKE_C_COMPILER_ID} "
  "${CMAKE_C_COMPILER_VERSION}")
message("  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message("")
message("  Dependencies:")
if(VFIO_USER_FOUND)
    message("    libvfio-user : "
      "${VFIO_USER_VERSION} (pkg-config)")
else()
    message("    libvfio-user : "
      "${VFIO_USER_FALLBACK_LIB} (fallback)")
endif()
message("    glib-2.0     : ${GLIB2_VERSION}")
message("    libibverbs   : ${IBVERBS_VERSION}")
message("")
message("  Sanitizers:")
message("    ASAN/LSAN/UBSAN : ${ERNIC_USE_SANITIZERS}")
message("    TSAN            : ${ERNIC_USE_THREAD_SANITIZER}")
message("")
message("  Kernel module (DKMS):")
message("    ERNIC_BUILD_KMOD : ${ERNIC_BUILD_KMOD}")
message("")
message("  rdma-core:")
message("    ERNIC_RDMA_CORE_BUILD :"
  " ${ERNIC_RDMA_CORE_BUILD}")
message("    RDMA_CORE_VERSION     :"
  " ${RDMA_CORE_VERSION}")
message("")
message("  Documentation:")
message("    Build docs : ${ERNIC_BUILD_DOCS}")
message("")
