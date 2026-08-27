# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# ErnicKernelModule.cmake
#
# Builds the ionic.ko Ethernet module and ionic_rdma.ko RDMA module
# from the upstream Linux kernel ionic driver sources, with rocm-ernic
# patches applied via git am.
#
# The ionic sources are fetched from a pinned kernel tag/SHA so the
# build is reproducible.  Bump IONIC_KERNEL_REF to track a new kernel
# release; re-verify that patches/*.patch still apply cleanly.
#
# Included from the top-level CMakeLists.txt.

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(ERNIC_BUILD_KMOD
    "Build and install ionic eth+RDMA DKMS modules for rocm-ernic" OFF)

# Pinned Linux kernel git reference for the ionic driver sources.
# Update this when moving to a newer upstream baseline.
set(IONIC_KERNEL_REF "v6.12"
    CACHE STRING
    "Linux kernel git tag or SHA to fetch ionic sources from")

set(IONIC_KERNEL_REPO
    "https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git"
    CACHE STRING
    "Linux kernel git repository URL")

# ---------------------------------------------------------------------------
# Derived paths
# ---------------------------------------------------------------------------

set(IONIC_SOURCE_DIR
    "${CMAKE_BINARY_DIR}/ionic-kernel-src")
set(IONIC_PATCHES_DIR
    "${CMAKE_SOURCE_DIR}/patches")
set(IONIC_DKMS_SCRIPT
    "${CMAKE_SOURCE_DIR}/scripts/setup-ionic-dkms.sh")

# ---------------------------------------------------------------------------
# Helper: fetch-and-patch target
# ---------------------------------------------------------------------------
# This custom target:
#   1. Sparse-clones only the two ionic subtrees from the pinned ref.
#   2. Applies patches/*.patch in filename order via git am.
# It is a dependency of the build/install targets so it runs once and
# is skipped on subsequent builds if the sentinel file exists.

set(IONIC_FETCH_SENTINEL
    "${IONIC_SOURCE_DIR}/.patches-applied-${IONIC_KERNEL_REF}")

if(ERNIC_BUILD_KMOD)
    # -- fetch-ionic-sources -------------------------------------------------
    add_custom_command(
        OUTPUT "${IONIC_FETCH_SENTINEL}"
        COMMAND ${CMAKE_COMMAND} -E echo
            "-- Fetching ionic sources at ${IONIC_KERNEL_REF}..."
        # Remove any previous checkout so the ref is always clean.
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${IONIC_SOURCE_DIR}"
        COMMAND git clone
            --depth 1
            --branch "${IONIC_KERNEL_REF}"
            --filter=blob:none
            --sparse
            "${IONIC_KERNEL_REPO}"
            "${IONIC_SOURCE_DIR}"
        # Sparse-checkout only the two ionic subtrees we need.
        COMMAND git -C "${IONIC_SOURCE_DIR}" sparse-checkout set
            "drivers/net/ethernet/pensando/ionic"
            "drivers/infiniband/hw/ionic"
        # Apply rocm-ernic patches in order.
        COMMAND ${CMAKE_COMMAND} -E echo
            "-- Applying rocm-ernic patches..."
        COMMAND bash -c
            "cd '${IONIC_SOURCE_DIR}' && \
             for p in $(ls '${IONIC_PATCHES_DIR}'/*.patch 2>/dev/null | sort); do \
                 echo \"  applying: $p\"; \
                 git am --whitespace=fix \"$p\" || exit 1; \
             done"
        COMMAND ${CMAKE_COMMAND} -E touch "${IONIC_FETCH_SENTINEL}"
        COMMENT
            "Fetching Linux ${IONIC_KERNEL_REF} ionic sources and applying patches"
        VERBATIM
    )

    add_custom_target(fetch-ionic-sources
        DEPENDS "${IONIC_FETCH_SENTINEL}"
        COMMENT "ionic sources fetched and patches applied"
    )

    # -- build-ionic-dkms ----------------------------------------------------
    add_custom_target(build-ionic-dkms
        DEPENDS fetch-ionic-sources
        COMMAND "${IONIC_DKMS_SCRIPT}"
            --source-dir "${IONIC_SOURCE_DIR}"
            --kernel-ref "${IONIC_KERNEL_REF}"
            --build-only
        COMMENT
            "Building ionic eth+RDMA DKMS modules (no install)"
        VERBATIM
    )

    # -- install-ionic-dkms --------------------------------------------------
    add_custom_target(install-ionic-dkms
        DEPENDS fetch-ionic-sources
        COMMAND "${IONIC_DKMS_SCRIPT}"
            --source-dir "${IONIC_SOURCE_DIR}"
            --kernel-ref "${IONIC_KERNEL_REF}"
        COMMENT
            "Building and installing ionic eth+RDMA DKMS modules"
        VERBATIM
    )

    # -- remove-ionic-dkms ---------------------------------------------------
    add_custom_target(remove-ionic-dkms
        COMMAND "${IONIC_DKMS_SCRIPT}"
            --source-dir "${IONIC_SOURCE_DIR}"
            --kernel-ref "${IONIC_KERNEL_REF}"
            --uninstall
        COMMENT "Removing ionic eth+RDMA DKMS modules"
        VERBATIM
    )

    message(STATUS
        "ernic: ionic DKMS targets enabled"
        " (ref=${IONIC_KERNEL_REF})")
endif()
