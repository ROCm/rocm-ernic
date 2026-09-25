# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

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

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

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
    # No NO_DEFAULT_PATH: the default search also covers
    # CMAKE_PREFIX_PATH, which is what the failure message below
    # tells the user to set for a non-standard prefix such as
    # ~/.local.
    find_library(VFIO_USER_FALLBACK_LIB
        NAMES vfio-user
        HINTS
            /usr/local/lib
            /usr/local/lib/x86_64-linux-gnu
            /usr/lib
            /usr/lib/x86_64-linux-gnu
        DOC "Path to libvfio-user library"
    )
    find_path(VFIO_USER_FALLBACK_INC
        NAMES vfio-user/libvfio-user.h
        HINTS
            /usr/local/include
            /usr/include
        DOC "Path to libvfio-user headers"
    )
    if(NOT VFIO_USER_FALLBACK_LIB OR
       NOT VFIO_USER_FALLBACK_INC)
        message(FATAL_ERROR
          "libvfio-user not found via pkg-config, in "
          "/usr/local, or on CMAKE_PREFIX_PATH.\n"
          "Install it from "
          "https://github.com/nutanix/libvfio-user\n"
          "then either add its pkgconfig dir to "
          "PKG_CONFIG_PATH or set CMAKE_PREFIX_PATH to its "
          "install prefix (e.g. -DCMAKE_PREFIX_PATH=$ENV{HOME}/.local)."
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

# ---------------------------------------------------------------
# Source file groups
# ---------------------------------------------------------------

# Upstream QEMU/VMware code, under Oracle / Red Hat copyright.
# Not ours to fix
set(ERNIC_VENDORED_SOURCES
    third-party/qemu/hw/rdma/rdma_backend.c
    third-party/qemu/hw/rdma/rdma_rm.c
    third-party/qemu/hw/rdma/rdma_utils.c
    third-party/qemu/hw/rdma/vmw/pvrdma_qp_ops.c
)

# The QEMU APIs the vendored code calls, implemented for a standalone
# process
set(QEMU_COMPAT_SOURCES
    src/qemu-compat/error-report.c
    src/qemu-compat/qemu_stubs.c
    src/qemu-compat/qemu_type_stubs.c
)

# RDMA backends (none, loopback, TCP mesh) behind the vendored device model
set(RDMA_BACKEND_SOURCES
    src/rdma/rdma_backend_core.c
    src/rdma/rdma_backend_none.c
    src/rdma/rdma_backend_loopback.c
    src/rdma/rdma_backend_tcp.c
    src/rdma/pvrdma_stats.c
)

# DHCP, RDMA CM and Ethernet frame injection for the emulated network
set(NET_SOURCES
    src/net/dhcp_server.c
    src/net/eth_rx_inject.c
    src/net/rdma_cm_proto.c
)

# Compatibility bridge layer
set(BRIDGE_SOURCES
    src/rocm_ernic_compat.c
)

# ionic emulation layer -- the device the guest actually sees
set(IONIC_EMU_SOURCES
    src/ionic_eth_emu.c
    src/ionic_eth_net.c
    src/ionic_rdma_devcmd.c
    src/ionic_adminq.c
    src/ionic_datapath.c
)

# In-process NVMe-oF controller reached over the emulated fabric
set(NVMEOF_SOURCES
    src/nvmeof_target.c
    src/nvmeof_cm.c
)

# In-process S3-over-RDMA object store, reached in-band over the emulated NIC
set(S3_SOURCES
    src/s3_token.c
    src/s3_http.c
    src/s3_target.c
    src/s3_tcp.c
)

# Main server implementation
set(SERVER_SOURCES
    src/rocm_ernic_server.c
)

set(ALL_SOURCES
    ${SERVER_SOURCES}
    ${BRIDGE_SOURCES}
    ${IONIC_EMU_SOURCES}
    ${NVMEOF_SOURCES}
    ${S3_SOURCES}
    ${RDMA_BACKEND_SOURCES}
    ${NET_SOURCES}
    ${QEMU_COMPAT_SOURCES}
    ${ERNIC_VENDORED_SOURCES}
)

# ---------------------------------------------------------------
# Executable
# ---------------------------------------------------------------
add_executable(rocm-ernic ${ALL_SOURCES})

# Resolve actual ibverbs include directory.
# pkg-config sometimes reports /usr/include as IBVERBS_INCLUDE_DIRS even
# when the actual headers live elsewhere (e.g. a local rdma-core build).
# Use IBVERBS_INCLUDEDIR (the canonical prefix) when it differs.
if(IBVERBS_INCLUDEDIR AND NOT "${IBVERBS_INCLUDEDIR}" STREQUAL "/usr/include")
    set(ERNIC_IBVERBS_INCLUDE_DIRS "${IBVERBS_INCLUDEDIR}")
else()
    set(ERNIC_IBVERBS_INCLUDE_DIRS "${IBVERBS_INCLUDE_DIRS}")
endif()

# src/rdma is searched directly because the vendored sources include
# rdma_backend_ops.h and pvrdma_comp_ctx.h by bare name.
target_include_directories(rocm-ernic PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/rdma
    ${CMAKE_SOURCE_DIR}/src/qemu-compat/include
    ${CMAKE_SOURCE_DIR}/shared
)

# The vendored header trees are SYSTEM includes so their warnings stay
# quiet
target_include_directories(rocm-ernic SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third-party/qemu/include
    ${CMAKE_SOURCE_DIR}/third-party/qemu/hw/rdma
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)

target_compile_definitions(rocm-ernic PRIVATE
    GIT_SHA_FULL="${GIT_SHA_FULL}"
    GIT_SHA_SHORT="${GIT_SHA_SHORT}"
    _GNU_SOURCE
)

target_compile_options(rocm-ernic PRIVATE
    ${GLIB2_CFLAGS_OTHER}
    -g
)

# Suppress strict warnings under third-party/
#
# Vendored code is not ours to fix.
set_source_files_properties(
    ${ERNIC_VENDORED_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-w"
)

target_link_directories(rocm-ernic PRIVATE
    ${VFIO_USER_LIBRARY_DIRS}
    ${GLIB2_LIBRARY_DIRS}
    ${IBVERBS_LIBRARY_DIRS}
)

target_link_libraries(rocm-ernic PRIVATE
    ${VFIO_USER_LINK_LIBRARIES}
    ${GLIB2_LIBRARIES}
    ${IBVERBS_LIBRARIES}
)

ernic_set_compiler_flags(rocm-ernic)

# Off by default so a downstream or distro build is never broken by a
# warning from a compiler version we have not seen. CI passes
# -DERNIC_WERROR=ON on every lane that compiles the project, so new
# warnings still fail the build where it matters.
option(ERNIC_WERROR
  "Treat compiler warnings as errors for project code"
  OFF)

if(ERNIC_WERROR)
    target_compile_options(rocm-ernic PRIVATE -Werror)
endif()

# ---------------------------------------------------------------
# Tests
# ---------------------------------------------------------------
add_subdirectory(tests)

# ---------------------------------------------------------------
# Kernel module (DKMS)
# ---------------------------------------------------------------
include(ErnicKernelModule)

# ---------------------------------------------------------------
# Install
# ---------------------------------------------------------------
# Must follow ErnicKernelModule: the install rules record
# IONIC_KERNEL_REF, which is defined there.
include(ErnicInstall)

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
message("  Documentation:")
message("    Build docs : ${ERNIC_BUILD_DOCS}")
message("")
