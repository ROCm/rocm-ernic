# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# DEB packages of rocm-ernic, made with CPack
#
# Configure with the prefix the packages install to, which is normally
# /usr, since the installed service files have the configured paths in
# them (see ernic_install_with_paths()). Then, in the build directory:
#
#   cpack -G DEB
#
# There's a package for each install component (see ErnicInstall):
#   rocm-ernic           ernic_server
#   rocm-ernic-exporter  ernic_exporter, with ERNIC_INSTALL_SERVICE
#
# The server needs libvfio-user, which Ubuntu doesn't package, so a
# package that's going to another machine should have its own copy
# (ERNIC_BUNDLE_VFIO_USER).
#
# Only a top-level build includes this, since CPack's configuration is
# global and a superproject has its own.

include_guard(GLOBAL)

set(ERNIC_PACKAGE_RELEASE "local" CACHE STRING
    "The release of the DEB packages, the part of their version after \
the project's version")
# A Debian revision: letters, digits, "+", ".", and "~". It also can't be
# a value that CMake takes as false, such as 0 or N: CPack would leave it
# out of the version, and the exporter's dependency on this release of
# the server could never be met.
if(NOT ERNIC_PACKAGE_RELEASE MATCHES "^[A-Za-z0-9.+~]+$")
    message(FATAL_ERROR
      "ERNIC_PACKAGE_RELEASE (\"${ERNIC_PACKAGE_RELEASE}\") must be one or "
      "more letters, digits, \"+\", \".\", and \"~\"")
endif()
if(NOT ERNIC_PACKAGE_RELEASE)
    message(FATAL_ERROR
      "ERNIC_PACKAGE_RELEASE (\"${ERNIC_PACKAGE_RELEASE}\") is a value "
      "CMake takes as false, which CPack would leave out of the packages' "
      "version. Use another, such as 1.")
endif()

set(CPACK_PACKAGE_NAME rocm-ernic)
set(CPACK_PACKAGE_VENDOR "Advanced Micro Devices, Inc.")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY ${PROJECT_DESCRIPTION})
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/ROCm/rocm-ernic")
set(CPACK_PACKAGE_CONTACT "rocm-ernic-maintainer@amd.com")

# Install into a staging directory with DESTDIR, under the configured
# prefix. Without DESTDIR, CPack would change the prefix instead, and
# the configuration file, which is outside it in /etc, would be
# installed on the machine building the packages.
set(CPACK_SET_DESTDIR ON)
set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

set(CPACK_COMPONENTS_ALL ernic_server)
if(ERNIC_INSTALL_SERVICE)
    list(APPEND CPACK_COMPONENTS_ALL ernic_exporter)
endif()
set(CPACK_DEB_COMPONENT_INSTALL ON)

# rocm-ernic_<version>-<release>_<architecture>.deb
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_RELEASE "${ERNIC_PACKAGE_RELEASE}")
set(CPACK_DEBIAN_PACKAGE_SECTION net)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

# Find the packages that provide the libraries the programs link with
# dpkg-shlibdeps. It follows the server's RPATH to the private
# libvfio-user in the package, and adds the libraries that needs.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# Each package's description follows the summary, which for every package
# is the project's description (CPACK_PACKAGE_DESCRIPTION_SUMMARY).

# rocm-ernic
#
# ernicctl is Python, and uses socat to talk to QEMU, but only for some
# commands
set(CPACK_DEBIAN_ERNIC_SERVER_PACKAGE_NAME rocm-ernic)
set(CPACK_DEBIAN_ERNIC_SERVER_DESCRIPTION
    "The rocm-ernic server, which emulates an RDMA NIC for virtual machines
over vfio-user, with its systemd service, ernicctl, and the guest driver
pack.")
if(ERNIC_INSTALL_SERVICE)
    set(CPACK_DEBIAN_ERNIC_SERVER_PACKAGE_DEPENDS python3)
    set(CPACK_DEBIAN_ERNIC_SERVER_PACKAGE_RECOMMENDS socat)

    # The site's configuration, which upgrades mustn't replace
    set(ernic_conffiles "${PROJECT_BINARY_DIR}/ernic-package/conffiles")
    file(WRITE "${ernic_conffiles}.tmp" "${ERNIC_ENV_FILE}\n")
    configure_file("${ernic_conffiles}.tmp" "${ernic_conffiles}" COPYONLY)
    set(CPACK_DEBIAN_ERNIC_SERVER_PACKAGE_CONTROL_EXTRA
      "${ernic_conffiles}")
endif()

# rocm-ernic-exporter
#
# It reads the server's statistics files, so it depends on the server
# from the same build. It's Python, so it's for every architecture.
# prometheus_client isn't a dependency: Ubuntu's is too old, so it's
# installed in a virtual environment (see docs/monitoring.rst), which the
# service runs the exporter from. Ubuntu's python3 can't make one without
# python3-venv.
set(CPACK_DEBIAN_ERNIC_EXPORTER_PACKAGE_NAME rocm-ernic-exporter)
set(CPACK_DEBIAN_ERNIC_EXPORTER_PACKAGE_ARCHITECTURE all)
set(CPACK_DEBIAN_ERNIC_EXPORTER_PACKAGE_DEPENDS
    "python3, python3-venv, rocm-ernic (= ${CPACK_PACKAGE_VERSION}-${ERNIC_PACKAGE_RELEASE})")
set(CPACK_DEBIAN_ERNIC_EXPORTER_DESCRIPTION
    "A Prometheus exporter for the statistics of the rocm-ernic server, with
its systemd service and a Grafana dashboard.")

include(CPack)
