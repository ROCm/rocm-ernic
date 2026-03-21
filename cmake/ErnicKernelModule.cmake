# Copyright (c) Advanced Micro Devices, Inc.
# All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# ErnicKernelModule.cmake
#
# Builds the rocm_ernic_eth.ko ethernet module
# and rocm_ernic_rdma.ko RDMA kernel module via
# DKMS.
#
# Included from the top-level CMakeLists.txt.

option(ERNIC_BUILD_KMOD
    "Setup rocm-ernic eth+RDMA DKMS modules" OFF)

set(ERNIC_DKMS_SETUP_SCRIPT
    "${CMAKE_SOURCE_DIR}/driver/setup-rocm-ernic-dkms.sh")

if(ERNIC_BUILD_KMOD)
    message(STATUS
        "ernic: DKMS kmod targets enabled"
        " (${ERNIC_DKMS_SETUP_SCRIPT})")

    add_custom_target(build-ernic-dkms
        COMMAND ${ERNIC_DKMS_SETUP_SCRIPT}
            --build-only
        COMMENT
            "Building rocm-ernic eth+RDMA DKMS"
            " modules (no install)"
        VERBATIM
    )

    add_custom_target(install-ernic-dkms
        COMMAND ${ERNIC_DKMS_SETUP_SCRIPT}
        COMMENT
            "Building and installing rocm-ernic"
            " eth+RDMA DKMS modules"
        VERBATIM
    )

    add_custom_target(remove-ernic-dkms
        COMMAND ${ERNIC_DKMS_SETUP_SCRIPT}
            --uninstall
        COMMENT
            "Removing rocm-ernic eth+RDMA DKMS"
            " modules"
        VERBATIM
    )
endif()
