# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Install targets for rocm-ernic.

include(GNUInstallDirs)

install(TARGETS rocm-ernic
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ── Optional systemd service installation ────────
option(ERNIC_INSTALL_SERVICE
    "Install systemd service files, ernicctl, \
and launcher"
    OFF
)

if(ERNIC_INSTALL_SERVICE)
    set(ERNIC_LIBEXEC_DIR
        "${CMAKE_INSTALL_PREFIX}/libexec/rocm-ernic"
    )
    set(ERNIC_SHARE_DIR
        "${CMAKE_INSTALL_PREFIX}/share/rocm-ernic"
    )

    # systemd unit files
    install(FILES
        ${CMAKE_SOURCE_DIR}/service/rocm-ernic.service
        ${CMAKE_SOURCE_DIR}/service/rocm-ernic-driver-pack.service
        DESTINATION lib/systemd/system
    )

    # Launcher and driver-pack scripts
    install(PROGRAMS
        ${CMAKE_SOURCE_DIR}/service/rocm-ernic-launcher
        ${CMAKE_SOURCE_DIR}/service/rocm-ernic-driver-pack
        DESTINATION ${ERNIC_LIBEXEC_DIR}
    )

    # ernicctl CLI
    install(PROGRAMS
        ${CMAKE_SOURCE_DIR}/service/ernicctl
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    # Prometheus exporter
    install(PROGRAMS
        ${CMAKE_SOURCE_DIR}/prometheus/ernic-exporter
        DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    # ernic-exporter systemd unit
    install(FILES
        ${CMAKE_SOURCE_DIR}/prometheus/ernic-exporter.service
        DESTINATION lib/systemd/system
    )

    # Exporter Python dependencies
    install(FILES
        ${CMAKE_SOURCE_DIR}/prometheus/requirements-exporter.txt
        DESTINATION ${ERNIC_SHARE_DIR}
    )

    # Grafana dashboard
    install(FILES
        ${CMAKE_SOURCE_DIR}/prometheus/grafana/ernic-dashboard.json
        DESTINATION ${ERNIC_SHARE_DIR}/grafana
    )

    # Default environment file
    install(FILES
        ${CMAKE_SOURCE_DIR}/service/rocm-ernic.env
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/rocm-ernic
    )

    # vm-driver-install template
    install(FILES
        ${CMAKE_SOURCE_DIR}/service/vm-driver-install.sh.in
        DESTINATION ${ERNIC_SHARE_DIR}
    )

    # Legacy custom driver source (deprecated; kept for reference until the
    # ionic migration is fully validated and driver/ is deleted).
    # New installations should use the ionic driver path (--ionic flag).
    if(EXISTS ${CMAKE_SOURCE_DIR}/driver/rocm_ernic_main.c)
        install(DIRECTORY
            ${CMAKE_SOURCE_DIR}/driver/
            DESTINATION ${ERNIC_SHARE_DIR}/driver-legacy
            FILES_MATCHING
                PATTERN "*.c"
                PATTERN "*.h"
                PATTERN "Makefile"
                PATTERN "dkms.conf"
                PATTERN "Kconfig"
                PATTERN "setup-rocm-ernic-dkms.sh"
                PATTERN ".*.cmd" EXCLUDE
                PATTERN "*.ko" EXCLUDE
                PATTERN "*.o" EXCLUDE
                PATTERN "*.mod" EXCLUDE
                PATTERN "*.mod.c" EXCLUDE
                PATTERN "Module.symvers" EXCLUDE
                PATTERN "modules.order" EXCLUDE
        )
    endif()

    # ionic driver patch and DKMS script (new path)
    install(DIRECTORY
        ${CMAKE_SOURCE_DIR}/patches/
        DESTINATION ${ERNIC_SHARE_DIR}/patches
        FILES_MATCHING PATTERN "*.patch"
    )
    install(PROGRAMS
        ${CMAKE_SOURCE_DIR}/scripts/setup-ionic-dkms.sh
        DESTINATION ${ERNIC_SHARE_DIR}
    )
endif()
