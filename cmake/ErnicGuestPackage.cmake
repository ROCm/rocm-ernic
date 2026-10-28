# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# DEB packages for the guest-side components of rocm-ernic, made with CPack.
#
# These packages are distinct from the server packages in ErnicPackage.cmake.
# They are installed *inside* a guest VM, not on the host running the server.
# See docs/p4.rst "Guest-side requirements" and the Ansible roles for context.
#
# Three packages are defined here (enabled by ERNIC_BUILD_GUEST_PACKAGES):
#
#   ionic-ernic-dkms          ernic_guest_srq_dkms
#     The SRQ-patched ionic kernel modules as a DKMS source tree. A guest
#     does: apt install ionic-ernic-dkms && dkms autoinstall
#
#   ionic-ernic-srq-provider  ernic_guest_srq_provider
#     The rdma-core SRQ provider (patches + build scripts). Installed under
#     /usr/share/ionic-ernic-srq-provider; the Ansible role or a postinst
#     script drives the build.
#
#   ionic-ernic-p4-dkms       ernic_guest_p4_dkms
#     ernic_p4.ko as a DKMS source package. Depends on ionic-ernic-dkms
#     being installed first (it creates the auxiliary device ernic_p4 binds to).
#
# RELATIONSHIP TO THE ANSIBLE ROLES. The Ansible roles (ernic_srq_dkms,
# ernic_srq_provider, ernic_p4_dkms) remain the fallback path for consumers
# who cannot or do not want to use APT. When these packages are available the
# roles should prefer them; that transition is tracked in the issue opened
# alongside this file.
#
# VERSIONING. All three packages carry PROJECT_VERSION, the same version as
# the rocm-ernic server package. Server/guest skew is then a dpkg version
# comparison, not a SHA comparison.
#
# HOW TO BUILD. Enable ERNIC_BUILD_GUEST_PACKAGES and run:
#   cmake -DERNIC_BUILD_GUEST_PACKAGES=ON -DERNIC_PACKAGE_RELEASE=1 ...
#   cmake --build .
#   cpack -G DEB --config CPackGuestConfig.cmake
#
# The separate CPackGuestConfig avoids colliding with the server CPack run.
#
# NOTE: This file is a scaffold. The install() targets in ErnicInstall.cmake
# that populate these components, the CI lane that builds and publishes the
# DEBs, and the install tests are tracked in the associated GitHub issue
# (see cmake/ErnicGuestPackage.cmake header). Do not call include(CPack)
# here yet: that call is in ErnicPackage.cmake and must happen only once per
# configure. Add the guest components to CPACK_COMPONENTS_ALL instead (see
# below), and split to a separate cpack invocation via CPackGuestConfig.cmake
# when the install targets exist.

include_guard(GLOBAL)

option(ERNIC_BUILD_GUEST_PACKAGES
    "Build and package the guest-side components: SRQ DKMS, SRQ provider, P4 DKMS"
    OFF
)

if(NOT ERNIC_BUILD_GUEST_PACKAGES)
    return()
endif()

# ---------------------------------------------------------------------------
# Shared package metadata (mirrors ErnicPackage.cmake conventions)
# ---------------------------------------------------------------------------

# All three packages carry the same version as the server, so server/guest
# skew is a dpkg --compare-versions check rather than a SHA comparison.
set(ERNIC_GUEST_PACKAGE_VERSION "${PROJECT_VERSION}"
    CACHE STRING
    "Version for guest-side packages (defaults to PROJECT_VERSION)")

# Release tag: letters, digits, "+", ".", "~", same rules as ERNIC_PACKAGE_RELEASE.
# Set on the CI to the build number; local builds use "local".
set(ERNIC_GUEST_PACKAGE_RELEASE "local" CACHE STRING
    "Release suffix for guest-side DEB packages")
if(NOT ERNIC_GUEST_PACKAGE_RELEASE MATCHES "^[A-Za-z0-9.+~]+$")
    message(FATAL_ERROR
      "ERNIC_GUEST_PACKAGE_RELEASE (\"${ERNIC_GUEST_PACKAGE_RELEASE}\") must be "
      "one or more letters, digits, \"+\", \".\", and \"~\"")
endif()

# Where the guest packages install their file trees on the target guest.
# /usr/share/<pkg> is the conventional home for DKMS source trees and
# provider patch sets; /usr/src/<pkg>-<version> is where dkms.conf points.
set(ERNIC_GUEST_SHARE_DIR "${CMAKE_INSTALL_DATADIR}"
    CACHE PATH
    "Installation prefix for guest package data (default: CMAKE_INSTALL_DATADIR)")

# ---------------------------------------------------------------------------
# ionic-ernic-dkms  (component: ernic_guest_srq_dkms)
# ---------------------------------------------------------------------------
#
# Ships the SRQ-patched ionic DKMS source tree:
#   - ansible/files/patches/          (base patch series, patches/srq included)
#   - ansible/files/scripts/          (fetch-ionic-sources.sh, setup-ionic-dkms.sh)
#   - cmake/ErnicKernelModule.cmake   (the IONIC_KERNEL_REF pin -- single source of truth)
#   - a dkms.conf generated at configure time
#
# TODO (tracked in issue): add install() targets for the above, wire in the
# IONIC_KERNEL_REF from ErnicKernelModule.cmake, and generate dkms.conf.
#
# Package name chosen to namespace clearly under ionic- (the upstream driver)
# while indicating ernic ownership.

set(ERNIC_DKMS_PACKAGE_NAME "ionic-ernic-dkms")
set(ERNIC_DKMS_PACKAGE_DESCRIPTION
    "DKMS source tree for the SRQ-capable ionic kernel modules required by
the rocm-ernic RDMA emulator. Installs the base and SRQ patch series against
the upstream ionic driver at the kernel ref pinned in the rocm-ernic server
package of the same version. Build with: dkms autoinstall -m ionic-ernic")

# ---------------------------------------------------------------------------
# ionic-ernic-srq-provider  (component: ernic_guest_srq_provider)
# ---------------------------------------------------------------------------
#
# Ships the rdma-core SRQ provider patch series and build scripts:
#   - ansible/files/patches/rdma-core/  (the SRQ provider patches)
#   - a postinst script that drives the cmake/ninja build against the local
#     rdma-core checkout (or triggers the Ansible role)
#
# TODO: add install() targets, postinst, and the base SHA validation
# (currently in ernic_srq_provider/tasks/main.yml).

set(ERNIC_SRQ_PROVIDER_PACKAGE_NAME "ionic-ernic-srq-provider")
set(ERNIC_SRQ_PROVIDER_PACKAGE_DESCRIPTION
    "The rdma-core SRQ provider patch series for the rocm-ernic RDMA emulator.
Adds create_srq support to the upstream ionic provider, which no released
rdma-core carries. Must be installed with a matching ionic-ernic-dkms package.")

# ---------------------------------------------------------------------------
# ionic-ernic-p4-dkms  (component: ernic_guest_p4_dkms)
# ---------------------------------------------------------------------------
#
# Ships the ernic_p4.ko DKMS source:
#   - drivers/ernic_p4/               (the module source)
#   - ansible/files/patches/p4/       (the P4 patch series on top of base)
#   - a dkms.conf generated at configure time
#
# Depends on ionic-ernic-dkms: the P4 module binds to an auxiliary device
# that the SRQ-patched ionic modules expose. Installing P4 without the SRQ
# modules loads ernic_p4.ko against the wrong Module.symvers and modprobe
# refuses it.
#
# TODO: add install() targets, generate dkms.conf from drivers/ernic_p4/dkms.conf,
# and set the inter-package dependency.

set(ERNIC_P4_DKMS_PACKAGE_NAME "ionic-ernic-p4-dkms")
set(ERNIC_P4_DKMS_PACKAGE_DESCRIPTION
    "DKMS source tree for ernic_p4.ko, the guest kernel module that binds to
the P4 engine exposed by the rocm-ernic server. Requires ionic-ernic-dkms
(same version) to be installed and its modules loaded before this module
will bind to anything. The server must also be started with --p4-guest-scope.")

# ---------------------------------------------------------------------------
# Wire components into CPack
#
# These are appended to CPACK_COMPONENTS_ALL in ErnicPackage.cmake by the
# top-level CMakeLists once both this file and ErnicPackage.cmake are
# included. Separation is intentional: the guest packages can be built
# independently of the server via -DERNIC_BUILD_GUEST_PACKAGES=ON without
# touching the server CPack configuration.
# ---------------------------------------------------------------------------

set(ERNIC_GUEST_CPACK_COMPONENTS
    ernic_guest_srq_dkms
    ernic_guest_srq_provider
    ernic_guest_p4_dkms
    CACHE INTERNAL "CPack components for guest-side packages")

# Surface a note so a developer configuring with ERNIC_BUILD_GUEST_PACKAGES=ON
# sees what is and is not implemented yet.
message(STATUS
    "rocm-ernic: guest packages enabled (ERNIC_BUILD_GUEST_PACKAGES=ON). "
    "Note: install() targets are not yet wired -- see cmake/ErnicGuestPackage.cmake "
    "and the associated GitHub issue for the remaining work.")
