# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Builds the ionic.ko Ethernet module and ionic_rdma.ko RDMA module
# from the upstream Linux kernel ionic driver sources, with rocm-ernic
# patches applied via git am.
#
# The ionic sources are fetched from a pinned kernel tag/SHA so the
# build is reproducible.  Bump IONIC_KERNEL_REF to track a new kernel
# release; re-verify that ansible/files/patches/*.patch still apply
# cleanly.
#
# Included from the top-level CMakeLists.txt.

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(ERNIC_BUILD_KMOD
  "Build and install ionic eth+RDMA DKMS modules for rocm-ernic" OFF)

# Pinned Linux kernel git reference for the ionic driver sources.
# Update this when moving to a newer upstream baseline.
#
# Must be >= v6.18: drivers/infiniband/hw/ionic (ionic_rdma.ko) was only
# merged for 6.18, so earlier refs cannot supply the RDMA half of the
# stack and the DKMS build fails with a missing source directory.
set(IONIC_KERNEL_REF "v7.2.6"
  CACHE STRING
  "Linux kernel git tag or SHA to fetch ionic sources from")

# Stable tree, so point releases (vX.Y.Z) as well as vX.Y are resolvable.
set(IONIC_KERNEL_REPO
  "https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"
  CACHE STRING
  "Linux kernel git repository URL")

# ---------------------------------------------------------------------------
# Derived paths
# ---------------------------------------------------------------------------

set(IONIC_SOURCE_DIR
  "${PROJECT_BINARY_DIR}/ionic-kernel-src")
# The patch series and the two scripts live in the Ansible collection, so
# that a consumer installing sbates130272.rocm_ernic gets them at the same
# ref as the roles that apply them.  See ansible/files/README.md.
set(IONIC_PATCHES_DIR
  "${PROJECT_SOURCE_DIR}/ansible/files/patches")
set(IONIC_DKMS_SCRIPT
  "${PROJECT_SOURCE_DIR}/ansible/files/scripts/setup-ionic-dkms.sh")
set(IONIC_FETCH_SCRIPT
  "${PROJECT_SOURCE_DIR}/ansible/files/scripts/fetch-ionic-sources.sh")

# ---------------------------------------------------------------------------
# Helper: fetch-and-patch target
# ---------------------------------------------------------------------------
# This custom target:
#   1. Sparse-clones only the two ionic subtrees from the pinned ref.
#   2. Applies ansible/files/patches/*.patch in filename order via git am.
# It is a dependency of the build/install targets.
#
# The target always runs, and the script decides whether there is work
# to do: it leaves a sentinel named for the ref and a digest of the
# repository and the patch series, and exits early when that sentinel is
# present, so changing IONIC_KERNEL_REF, IONIC_KERNEL_REPO or a patch
# fetches the sources again.  CMake can't name the sentinel as an
# OUTPUT, since the digest is computed by the script, and passing --force
# would make the script delete and fetch the tree again on every build.

if(ERNIC_BUILD_KMOD)
    # -- fetch-ionic-sources -------------------------------------------------
    add_custom_target(fetch-ionic-sources
      COMMAND "${IONIC_FETCH_SCRIPT}"
        --source-dir "${IONIC_SOURCE_DIR}"
        --kernel-ref "${IONIC_KERNEL_REF}"
        --patches-dir "${IONIC_PATCHES_DIR}"
        --repo "${IONIC_KERNEL_REPO}"
      COMMENT
        "Fetching and patching Linux ${IONIC_KERNEL_REF} ionic sources if needed"
      VERBATIM
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
