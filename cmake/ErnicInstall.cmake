# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Install targets for rocm-ernic.

include(GNUInstallDirs)

install(TARGETS rocm-ernic
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
