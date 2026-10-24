# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Install targets for rocm-ernic.
#
# Everything is installed in one of two components, which ErnicPackage
# turns into packages:
#   ernic_server    The server, with libvfio-user if
#                   ERNIC_BUNDLE_VFIO_USER is on, and with
#                   ERNIC_INSTALL_SERVICE, the service, ernicctl, and
#                   the guest driver pack
#   ernic_exporter  The Prometheus exporter (ERNIC_INSTALL_SERVICE only)

include_guard(GLOBAL)

include(GNUInstallDirs)

install(TARGETS rocm-ernic
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT ernic_server
)

install(FILES
    ${PROJECT_SOURCE_DIR}/LICENSE.md
    ${PROJECT_SOURCE_DIR}/LICENSE_GPL.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    COMPONENT ernic_server
)

# ── Optional private copy of libvfio-user ────────
#
# libvfio-user has no stable ABI: its soname stays libvfio-user.so.0
# through incompatible changes. So a package can't use a system copy, and
# must not install its own copy where other programs would find it. This
# installs the libvfio-user the server was linked with into a directory
# of its own, which only the installed server searches.
option(ERNIC_BUNDLE_VFIO_USER
    "Install a private copy of libvfio-user that only the installed \
server uses, for packages"
    OFF
)

if(ERNIC_BUNDLE_VFIO_USER)
    set(ERNIC_VFIO_USER_LICENSE "" CACHE FILEPATH
        "libvfio-user's license, installed with the private copy of \
libvfio-user (ERNIC_BUNDLE_VFIO_USER)")
    if(NOT EXISTS "${ERNIC_VFIO_USER_LICENSE}")
        message(FATAL_ERROR
          "ERNIC_BUNDLE_VFIO_USER needs ERNIC_VFIO_USER_LICENSE set to "
          "libvfio-user's LICENSE file, from the source it was built "
          "from")
    endif()

    # The library that the server links (the libvfio-user.so link), and
    # the file it points to. When pkg-config found libvfio-user, the
    # server links it by name (-lvfio-user), so look where the linker
    # does: the directories pkg-config gave, then the linker's own.
    unset(ernic_vfio_user_lib)
    foreach(lib IN LISTS VFIO_USER_LINK_LIBRARIES)
        if(IS_ABSOLUTE "${lib}" AND lib MATCHES "/libvfio-user\\.")
            set(ernic_vfio_user_lib "${lib}")
        endif()
    endforeach()
    if(NOT DEFINED ernic_vfio_user_lib)
        find_library(ernic_vfio_user_lib
            NAMES vfio-user
            HINTS ${VFIO_USER_LIBRARY_DIRS} ${CMAKE_C_IMPLICIT_LINK_DIRECTORIES}
            NO_DEFAULT_PATH
            NO_CACHE
        )
    endif()
    if(NOT ernic_vfio_user_lib MATCHES "\\.so$")
        message(FATAL_ERROR
          "ERNIC_BUNDLE_VFIO_USER needs the shared libvfio-user, and "
          "can't find it in the libraries the server links: "
          "${VFIO_USER_LINK_LIBRARIES}")
    endif()
    file(REAL_PATH "${ernic_vfio_user_lib}" ernic_vfio_user_file)

    if(NOT CMAKE_READELF)
        message(FATAL_ERROR "ERNIC_BUNDLE_VFIO_USER needs readelf")
    endif()
    execute_process(
        COMMAND ${CMAKE_READELF} -W -d -l "${ernic_vfio_user_file}"
        OUTPUT_VARIABLE ernic_vfio_user_elf
        RESULT_VARIABLE result
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
          "readelf can't read ${ernic_vfio_user_file}")
    endif()

    # The file is installed under its soname, the name the server looks
    # for
    if(NOT ernic_vfio_user_elf MATCHES
            "\\(SONAME\\)[^\n]*\\[([^]\n]+)\\]")
        message(FATAL_ERROR "${ernic_vfio_user_file} has no soname")
    endif()
    set(ernic_vfio_user_soname "${CMAKE_MATCH_1}")

    # The library is installed with the server, so it has to be hardened
    # like the server (see ernic_set_linker_flags()). Its link options
    # come from however it was built, so check them: full RELRO is a
    # GNU_RELRO segment and BIND_NOW.
    if(NOT ernic_vfio_user_elf MATCHES "GNU_RELRO"
            OR NOT ernic_vfio_user_elf MATCHES "BIND_NOW|Flags:[^\n]* NOW")
        message(FATAL_ERROR
          "${ernic_vfio_user_file} isn't linked with full RELRO. Build "
          "libvfio-user with LDFLAGS=\"-Wl,-z,relro,-z,now\".")
    endif()

    set(ernic_vfio_user_dir "${CMAKE_INSTALL_LIBDIR}/rocm-ernic")
    install(FILES "${ernic_vfio_user_file}"
        DESTINATION ${ernic_vfio_user_dir}
        RENAME ${ernic_vfio_user_soname}
        COMPONENT ernic_server
    )
    install(FILES "${ERNIC_VFIO_USER_LICENSE}"
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/libvfio-user
        COMPONENT ernic_server
    )

    # A library built outside /usr usually has an RPATH to its own
    # directory, which on another machine could be anyone's. It only
    # needs the system's libraries, so remove it. The prefix is the one
    # being installed to, unless CMAKE_INSTALL_LIBDIR is absolute.
    if(IS_ABSOLUTE "${ernic_vfio_user_dir}")
        set(ernic_vfio_user_installed "${ernic_vfio_user_dir}")
    else()
        set(ernic_vfio_user_installed
          "\${CMAKE_INSTALL_PREFIX}/${ernic_vfio_user_dir}")
    endif()
    string(APPEND ernic_vfio_user_installed "/${ernic_vfio_user_soname}")
    install(CODE "
        file(RPATH_REMOVE FILE
          \"\$ENV{DESTDIR}${ernic_vfio_user_installed}\")
        "
        COMPONENT ernic_server
    )

    # The installed server finds the library through an RPATH relative to
    # its own location. The server in the build tree keeps CMake's build
    # RPATH, to the libvfio-user it was linked with.
    #
    # The RPATH is a DT_RPATH and not the DT_RUNPATH the linker writes by
    # default, because LD_LIBRARY_PATH overrides a DT_RUNPATH. With one, a
    # developer's LD_LIBRARY_PATH to their own libvfio-user build, which has
    # the same soname but may not have the same ABI, would replace the
    # server's copy.
    file(RELATIVE_PATH ernic_vfio_user_rpath
        "${CMAKE_INSTALL_FULL_BINDIR}"
        "${CMAKE_INSTALL_FULL_LIBDIR}/rocm-ernic")
    set_target_properties(rocm-ernic PROPERTIES
        INSTALL_RPATH "$ORIGIN/${ernic_vfio_user_rpath}")
    target_link_options(rocm-ernic PRIVATE "LINKER:--disable-new-dtags")
endif()

# ── Optional systemd service installation ────────
option(ERNIC_INSTALL_SERVICE
    "Install systemd service files, ernicctl, \
and launcher"
    OFF
)

# Install a service file, with the paths of this install
#
# The files in service/ and prometheus/ refer to each other by where a
# default install, to /usr/local, puts them, so they also work from a
# source tree. In the installed copy, those paths are replaced with the
# ones for this install. These come from CMAKE_INSTALL_PREFIX when the
# project is configured, so installing to another prefix with
# cmake --install --prefix leaves them pointing at the configured one.
#
#   type         PROGRAMS or FILES, as for install()
#   source       The file, relative to the source directory
#   destination  The install directory, as for install()
#   component    The install component
function(ernic_install_with_paths type source destination component)
    file(READ "${PROJECT_SOURCE_DIR}/${source}" text)
    string(REPLACE "/usr/local/bin/"
        "${CMAKE_INSTALL_FULL_BINDIR}/" text "${text}")
    string(REPLACE "/usr/local/libexec/rocm-ernic"
        "${CMAKE_INSTALL_FULL_LIBEXECDIR}/rocm-ernic" text "${text}")
    string(REPLACE "/usr/local/share/rocm-ernic"
        "${CMAKE_INSTALL_FULL_DATADIR}/rocm-ernic" text "${text}")

    # Write the file through configure_file(), which only changes it when
    # its contents change, so it isn't reinstalled on every configure
    set(staged "${PROJECT_BINARY_DIR}/ernic-install/${source}")
    file(WRITE "${staged}.tmp" "${text}")
    configure_file("${staged}.tmp" "${staged}" COPYONLY)
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY
        CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/${source}")

    install(${type} "${staged}"
        DESTINATION ${destination}
        COMPONENT ${component}
    )
endfunction()

if(ERNIC_INSTALL_SERVICE)
    set(ERNIC_LIBEXEC_DIR "${CMAKE_INSTALL_LIBEXECDIR}/rocm-ernic")
    set(ERNIC_SHARE_DIR "${CMAKE_INSTALL_DATADIR}/rocm-ernic")

    # The configuration goes in /etc, where the services and ernicctl
    # read it, when the prefix is /usr. GNUInstallDirs only says so in
    # CMAKE_INSTALL_FULL_SYSCONFDIR; CMAKE_INSTALL_SYSCONFDIR is still
    # etc, which would put it in /usr/etc. Under other prefixes it's
    # under the prefix, and has to be copied to /etc (see
    # docs/service.rst).
    if(CMAKE_INSTALL_FULL_SYSCONFDIR STREQUAL
            "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_SYSCONFDIR}")
        set(ERNIC_SYSCONF_DIR "${CMAKE_INSTALL_SYSCONFDIR}/rocm-ernic")
    else()
        set(ERNIC_SYSCONF_DIR
          "${CMAKE_INSTALL_FULL_SYSCONFDIR}/rocm-ernic")
    endif()
    # The installed file, for the package's list of configuration files
    set(ERNIC_ENV_FILE
      "${CMAKE_INSTALL_FULL_SYSCONFDIR}/rocm-ernic/rocm-ernic.env")

    # systemd unit files
    ernic_install_with_paths(FILES service/rocm-ernic.service
        lib/systemd/system ernic_server)
    ernic_install_with_paths(FILES service/rocm-ernic-driver-pack.service
        lib/systemd/system ernic_server)

    # Launcher and driver-pack scripts
    ernic_install_with_paths(PROGRAMS service/rocm-ernic-launcher
        ${ERNIC_LIBEXEC_DIR} ernic_server)
    ernic_install_with_paths(PROGRAMS service/rocm-ernic-driver-pack
        ${ERNIC_LIBEXEC_DIR} ernic_server)

    # ernicctl CLI
    ernic_install_with_paths(PROGRAMS service/ernicctl
        ${CMAKE_INSTALL_BINDIR} ernic_server)

    # Prometheus exporter
    ernic_install_with_paths(PROGRAMS prometheus/ernic-exporter
        ${CMAKE_INSTALL_BINDIR} ernic_exporter)

    # ernic-exporter systemd unit
    ernic_install_with_paths(FILES prometheus/ernic-exporter.service
        lib/systemd/system ernic_exporter)

    # Exporter Python dependencies
    install(FILES
        ${PROJECT_SOURCE_DIR}/prometheus/requirements-exporter.txt
        DESTINATION ${ERNIC_SHARE_DIR}
        COMPONENT ernic_exporter
    )

    # Grafana dashboard
    install(FILES
        ${PROJECT_SOURCE_DIR}/prometheus/grafana/ernic-dashboard.json
        DESTINATION ${ERNIC_SHARE_DIR}/grafana
        COMPONENT ernic_exporter
    )

    # The exporter's package has its own copy of the license
    install(FILES
        ${PROJECT_SOURCE_DIR}/LICENSE.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/rocm-ernic-exporter
        COMPONENT ernic_exporter
    )

    # Default environment file
    ernic_install_with_paths(FILES service/rocm-ernic.env
        ${ERNIC_SYSCONF_DIR} ernic_server)

    # vm-driver-install template
    install(FILES
        ${PROJECT_SOURCE_DIR}/service/vm-driver-install.sh.in
        DESTINATION ${ERNIC_SHARE_DIR}
        COMPONENT ernic_server
    )

    # ionic driver patches and helper scripts.  They live in the Ansible
    # collection (ansible/files/, see its README) so a consumer of the
    # collection gets them at the ref of the roles that apply them; the
    # installed layout under ERNIC_SHARE_DIR is what it always was, which
    # is what the driver pack and the in-VM installer see.
    install(DIRECTORY
        ${PROJECT_SOURCE_DIR}/ansible/files/patches/
        DESTINATION ${ERNIC_SHARE_DIR}/patches
        COMPONENT ernic_server
        FILES_MATCHING PATTERN "*.patch"
    )
    install(PROGRAMS
        ${PROJECT_SOURCE_DIR}/ansible/files/scripts/setup-ionic-dkms.sh
        ${PROJECT_SOURCE_DIR}/ansible/files/scripts/fetch-ionic-sources.sh
        DESTINATION ${ERNIC_SHARE_DIR}
        COMPONENT ernic_server
    )

    # The pinned ionic ref, materialised so the driver pack ships the
    # baseline the host was configured with rather than a second
    # hardcoded copy of it.
    if(NOT IONIC_KERNEL_REF)
        message(FATAL_ERROR
            "IONIC_KERNEL_REF is not set; include ErnicKernelModule "
            "before ErnicInstall")
    endif()
    file(GENERATE
        OUTPUT ${PROJECT_BINARY_DIR}/ionic-kernel-ref
        CONTENT "${IONIC_KERNEL_REF}\n"
    )
    install(FILES
        ${PROJECT_BINARY_DIR}/ionic-kernel-ref
        DESTINATION ${ERNIC_SHARE_DIR}
        COMPONENT ernic_server
    )
endif()
