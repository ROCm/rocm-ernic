# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Build rocm-ernic's DEB packages and check them
#
# Usage:
#   cmake -DSETTINGS=<settings.cmake> [-DCONFIG=<config>]
#     -P run-package-test.cmake
#
# SETTINGS is written by CMakeLists.txt in this directory. CONFIG is the
# build configuration to package, for multi-config generators. The
# packages are those of the build's configuration (see
# ErnicInstall.cmake and ErnicPackage.cmake), and the checks follow it.
#
# Checks:
#   - The packages: rocm-ernic, and rocm-ernic-exporter with
#     ERNIC_INSTALL_SERVICE, and no others
#   - Their version, <project version>-<ERNIC_PACKAGE_RELEASE>, and
#     architecture: the build machine's for rocm-ernic, and all for the
#     exporter, which is Python
#   - The files in each package, exactly. Everything installed is in the
#     package it belongs to, and nothing else is, such as a test program
#     or a README from the driver patches. With ERNIC_BUNDLE_VFIO_USER,
#     that includes the private libvfio-user and its license.
#   - The files' modes: 755 for programs and 644 for everything else
#   - What the packages depend on and recommend. rocm-ernic has to depend
#     on a package with each library that the server, or its private
#     libvfio-user, needs: dpkg-shlibdeps leaves out a library it can't
#     find a package for. The exporter depends on exactly this build's
#     rocm-ernic.
#   - That rocm-ernic.env, with ERNIC_INSTALL_SERVICE, is the only
#     configuration file, which upgrades keep
#   - That the installed files name where the packages install things.
#     The service files name /usr/local, which is replaced when they're
#     installed elsewhere, so no file names it unless that's the prefix
#     or the configuration's directory is under it.
#     Every program a systemd unit runs from under the prefix is in a
#     package. The units, and every file that names a rocm-ernic.env,
#     read the packaged configuration when it's outside the prefix (in
#     /etc with the prefix /usr, or /etc/opt/<name> with /opt/<name>),
#     and /etc/rocm-ernic/rocm-ernic.env when it's under it.
#   - With ERNIC_BUNDLE_VFIO_USER, that the server finds its libvfio-user
#     through a DT_RPATH, which LD_LIBRARY_PATH can't override, and that
#     the library has no run path of its own

cmake_minimum_required(VERSION 3.25.2)

if(NOT SETTINGS)
    message(FATAL_ERROR "run-package-test: SETTINGS is not set")
endif()
include(${SETTINGS})

# Fail the test with a message. The message can't contain semicolons,
# so lists in it have to be joined first.
function(ernic_fail)
    string(JOIN "" text ${ARGN})
    message(FATAL_ERROR "run-package-test: ${text}")
endfunction()

# Run a command, fail with its output if it fails, and set out_var to
# its standard output
function(ernic_run_command out_var description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        ernic_fail("${description} failed:\n${output}${error}")
    endif()
    set(${out_var} "${output}" PARENT_SCOPE)
endfunction()

set(package_dir ${TEST_DIR}/packages)
set(root ${TEST_DIR}/root)

file(REMOVE_RECURSE ${TEST_DIR})
file(MAKE_DIRECTORY ${TEST_DIR})

set(config_args)
if(CONFIG)
    set(config_args -C ${CONFIG})
endif()
ernic_run_command(unused "cpack -G DEB"
    ${CMAKE_CPACK_COMMAND} --config ${CPACK_CONFIG} -G DEB
        -B ${package_dir} ${config_args})

ernic_run_command(build_architecture "dpkg --print-architecture"
    ${DPKG} --print-architecture)
string(STRIP "${build_architecture}" build_architecture)

# ── What each package should be ──────────────────
#
# Keep these lists in step with the install rules in ErnicInstall.cmake.
# Each file is an absolute path, as the package installs it.
set(units_dir ${INSTALL_PREFIX}/lib/systemd/system)
set(share_dir ${DATADIR}/rocm-ernic)

set(server rocm-ernic)
set(${server}_architecture ${build_architecture})
set(${server}_programs ${BINDIR}/rocm-ernic)
set(${server}_files
    ${DOCDIR}/LICENSE.md
    ${DOCDIR}/LICENSE_GPL.md)
set(${server}_depends)
set(${server}_recommends)
set(${server}_conffiles)

if(BUNDLE_VFIO_USER)
    cmake_path(GET VFIO_USER_LICENSE FILENAME vfio_user_license_name)
    set(private_vfio_user ${LIBDIR}/rocm-ernic/${VFIO_USER_SONAME})
    list(APPEND ${server}_files
        ${private_vfio_user}
        ${DOCDIR}/libvfio-user/${vfio_user_license_name})
endif()

set(expected_packages ${server})

if(INSTALL_SERVICE)
    list(APPEND ${server}_programs
        ${BINDIR}/ernicctl
        ${LIBEXECDIR}/rocm-ernic/rocm-ernic-launcher
        ${LIBEXECDIR}/rocm-ernic/rocm-ernic-driver-pack
        ${share_dir}/setup-ionic-dkms.sh
        ${share_dir}/fetch-ionic-sources.sh)
    list(APPEND ${server}_files
        ${units_dir}/rocm-ernic.service
        ${units_dir}/rocm-ernic-driver-pack.service
        ${ENV_FILE}
        ${share_dir}/vm-driver-install.sh.in
        ${share_dir}/ionic-kernel-ref)
    # Only the patches from the driver patches directory, not its READMEs
    file(GLOB_RECURSE patches
        LIST_DIRECTORIES false
        RELATIVE ${SOURCE_DIR}/ansible/files/patches
        ${SOURCE_DIR}/ansible/files/patches/*.patch)
    if(NOT patches)
        ernic_fail("found no driver patches in "
            "${SOURCE_DIR}/ansible/files/patches")
    endif()
    foreach(patch IN LISTS patches)
        list(APPEND ${server}_files ${share_dir}/patches/${patch})
    endforeach()
    set(${server}_depends python3)
    set(${server}_recommends socat)
    set(${server}_conffiles ${ENV_FILE})

    set(exporter rocm-ernic-exporter)
    list(APPEND expected_packages ${exporter})
    set(${exporter}_architecture all)
    set(${exporter}_programs ${BINDIR}/ernic-exporter)
    set(${exporter}_files
        ${units_dir}/ernic-exporter.service
        ${share_dir}/requirements-exporter.txt
        ${share_dir}/grafana/ernic-dashboard.json
        ${DATAROOTDIR}/doc/rocm-ernic-exporter/LICENSE.md)
    set(${exporter}_depends python3 python3-venv)
    set(${exporter}_recommends)
    set(${exporter}_conffiles)
endif()

# ── Read the packages ────────────────────────────

# Turn a DEB relationship field into a list of package names, without
# version constraints or architecture qualifiers:
# "pkg (>= 1), a | b:any" -> "pkg;a;b"
function(ernic_parse_deb_relations out_var text)
    string(REGEX REPLACE "\\([^)]*\\)" "" text "${text}")
    string(REGEX REPLACE ":[a-z0-9]+" "" text "${text}")
    string(REGEX REPLACE "[ \t\n]" "" text "${text}")
    string(REGEX REPLACE "[,|]" ";" text "${text}")
    set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

# Read one field of a package's control file into out_var
function(ernic_read_field out_var package field)
    ernic_run_command(value "read ${field} from ${package}"
        ${DPKG_DEB} -f ${package} ${field})
    string(STRIP "${value}" value)
    set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

file(GLOB packages ${package_dir}/*.deb)
if(NOT packages)
    ernic_fail("cpack -G DEB didn't create any packages")
endif()

set(found_packages)
set(all_files)
foreach(package IN LISTS packages)
    ernic_read_field(name ${package} Package)
    cmake_path(GET package FILENAME package_file)
    set(what "${package_file} (${name})")

    if(NOT name IN_LIST expected_packages)
        list(JOIN expected_packages ", " expected_text)
        ernic_fail("${what}: unexpected package (expected ${expected_text})")
    endif()
    if(name IN_LIST found_packages)
        ernic_fail("cpack -G DEB created more than one ${name} package")
    endif()
    list(APPEND found_packages ${name})

    ernic_read_field(version ${package} Version)
    if(NOT version STREQUAL "${VERSION}")
        ernic_fail("${what}: version is ${version}, not ${VERSION}")
    endif()

    ernic_read_field(architecture ${package} Architecture)
    if(NOT architecture STREQUAL "${${name}_architecture}")
        ernic_fail("${what}: architecture is ${architecture}, not "
            "${${name}_architecture}")
    endif()

    ernic_read_field(depends ${package} Depends)
    set(${name}_depends_field "${depends}")
    ernic_parse_deb_relations(depends "${depends}")
    set(${name}_found_depends ${depends})
    foreach(depend IN LISTS ${name}_depends)
        if(NOT depend IN_LIST depends)
            ernic_fail("${what}: doesn't depend on ${depend} (Depends: "
                "${${name}_depends_field})")
        endif()
    endforeach()

    ernic_read_field(recommends ${package} Recommends)
    ernic_parse_deb_relations(recommends "${recommends}")
    set(expected "${${name}_recommends}")
    list(SORT recommends)
    list(SORT expected)
    if(NOT "${recommends}" STREQUAL "${expected}")
        list(JOIN recommends ", " recommends_text)
        list(JOIN expected ", " expected_text)
        ernic_fail("${what}: recommends '${recommends_text}', not "
            "'${expected_text}'")
    endif()

    # The configuration files are listed one per line in the control
    # archive's conffiles, which is there only if there are any
    set(control_dir ${TEST_DIR}/control/${name})
    file(MAKE_DIRECTORY ${TEST_DIR}/control)
    ernic_run_command(unused "extract the control files of ${package_file}"
        ${DPKG_DEB} -e ${package} ${control_dir})
    set(conffiles)
    if(EXISTS ${control_dir}/conffiles)
        file(STRINGS ${control_dir}/conffiles conffiles)
    endif()
    set(expected "${${name}_conffiles}")
    if(NOT "${conffiles}" STREQUAL "${expected}")
        list(JOIN conffiles ", " conffiles_text)
        list(JOIN expected ", " expected_text)
        ernic_fail("${what}: configuration files are '${conffiles_text}', not "
            "'${expected_text}'")
    endif()

    # <mode> <owner> <size> <date> <time> ./<path>. Directories end with
    # "/". Their modes aren't checked: CPack takes them from the staged
    # install, and so from the umask of whoever builds the packages.
    ernic_run_command(listing "list ${package_file}" ${DPKG_DEB} -c ${package})
    string(REPLACE "\n" ";" listing "${listing}")
    set(expected ${${name}_programs} ${${name}_files})
    set(files)
    foreach(line IN LISTS listing)
        if(line STREQUAL "")
            continue()
        endif()
        if(NOT line MATCHES
                "^(.)([^ ]+) +[^ ]+ +[0-9]+ +[^ ]+ +[^ ]+ +\\.(/.*)$")
            ernic_fail("${what}: can't read this line of its listing: ${line}")
        endif()
        set(type "${CMAKE_MATCH_1}")
        set(mode "${CMAKE_MATCH_2}")
        set(path "${CMAKE_MATCH_3}")
        if(type STREQUAL "d")
            continue()
        endif()
        if(NOT type STREQUAL "-")
            ernic_fail("${what}: ${path} isn't a regular file or a directory")
        endif()
        if(path IN_LIST all_files)
            ernic_fail("${what}: ${path} is in another package too")
        endif()
        if(NOT path IN_LIST expected)
            ernic_fail("${what}: contains ${path}, which it shouldn't")
        endif()
        if(path IN_LIST ${name}_programs)
            set(expected_mode rwxr-xr-x)
        else()
            set(expected_mode rw-r--r--)
        endif()
        if(NOT mode STREQUAL expected_mode)
            ernic_fail("${what}: ${path} has mode ${mode}, not "
                "${expected_mode}")
        endif()
        list(APPEND files "${path}")
        list(APPEND all_files "${path}")
    endforeach()

    foreach(file IN LISTS expected)
        if(NOT file IN_LIST files)
            ernic_fail("${what}: doesn't contain ${file}")
        endif()
    endforeach()

    ernic_run_command(unused "extract ${package_file}"
        ${DPKG_DEB} -x ${package} ${root})

    message(STATUS "run-package-test: ${what}: OK")
endforeach()

foreach(name IN LISTS expected_packages)
    if(NOT name IN_LIST found_packages)
        ernic_fail("cpack -G DEB didn't create ${name}")
    endif()
endforeach()

# The exporter reads the server's statistics files, so it has to depend
# on the server from the same build
if(INSTALL_SERVICE)
    set(exact_depend "${server} (= ${VERSION})")
    string(REPLACE "," ";" entries "${${exporter}_depends_field}")
    set(found OFF)
    foreach(entry IN LISTS entries)
        string(STRIP "${entry}" entry)
        if(entry STREQUAL exact_depend)
            set(found ON)
        endif()
    endforeach()
    if(NOT found)
        ernic_fail("${exporter} doesn't depend on ${exact_depend} (Depends: "
            "${${exporter}_depends_field})")
    endif()
endif()

# ── Libraries and run paths ──────────────────────

# Set out_var to the libraries an ELF file needs, and rpath_var and
# runpath_var to its DT_RPATH and DT_RUNPATH, empty if it has none
function(ernic_read_dynamic out_var rpath_var runpath_var file)
    ernic_run_command(dynamic "readelf -d ${file}" ${READELF} -W -d ${file})
    string(REGEX MATCHALL "\\(NEEDED\\)[^\n]*\\[[^]\n]+\\]" needed
        "${dynamic}")
    list(TRANSFORM needed REPLACE "^.*\\[([^]]+)\\]$" "\\1")
    set(rpath)
    if(dynamic MATCHES "\\(RPATH\\)[^\n]*\\[([^]\n]*)\\]")
        set(rpath "${CMAKE_MATCH_1}")
    endif()
    set(runpath)
    if(dynamic MATCHES "\\(RUNPATH\\)[^\n]*\\[([^]\n]*)\\]")
        set(runpath "${CMAKE_MATCH_1}")
    endif()
    set(${out_var} "${needed}" PARENT_SCOPE)
    set(${rpath_var} "${rpath}" PARENT_SCOPE)
    set(${runpath_var} "${runpath}" PARENT_SCOPE)
endfunction()

# Check that rocm-ernic depends on a package that has each library that
# an ELF file in it needs. The private libvfio-user is in rocm-ernic
# itself. Without it, nothing can provide libvfio-user, since Ubuntu
# doesn't package it, so it's left out.
function(ernic_check_library_depends file needed)
    foreach(library IN LISTS needed)
        if(BUNDLE_VFIO_USER AND library STREQUAL VFIO_USER_SONAME)
            continue()
        endif()
        if(NOT BUNDLE_VFIO_USER AND library MATCHES "^libvfio-user\\.")
            continue()
        endif()

        # "<package>[:<arch>][, <package>...]: <path>", one line for each
        # path that ends with the library's name
        execute_process(
            COMMAND ${DPKG_QUERY} -S "*/${library}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE owners_text
            ERROR_VARIABLE error)
        if(NOT result EQUAL 0)
            ernic_fail("${file} needs ${library}, which no installed package "
                "has, so rocm-ernic can't depend on it:\n${error}")
        endif()
        string(REPLACE "\n" ";" lines "${owners_text}")
        set(owners)
        foreach(line IN LISTS lines)
            string(FIND "${line}" ": /" separator)
            if(separator LESS 0 OR line MATCHES "^diversion ")
                continue()
            endif()
            string(SUBSTRING "${line}" 0 ${separator} line_owners)
            ernic_parse_deb_relations(line_owners "${line_owners}")
            list(APPEND owners ${line_owners})
        endforeach()

        set(found OFF)
        foreach(owner IN LISTS owners)
            if(owner IN_LIST ${server}_found_depends)
                set(found ON)
            endif()
        endforeach()
        if(NOT found)
            list(REMOVE_DUPLICATES owners)
            list(JOIN owners ", " owners_text)
            ernic_fail("${file} needs ${library}, but ${server} doesn't "
                "depend on a package that has it (${owners_text}). Depends: "
                "${${server}_depends_field}")
        endif()
    endforeach()
endfunction()

set(server_program ${root}${BINDIR}/rocm-ernic)
ernic_read_dynamic(needed rpath runpath ${server_program})
ernic_check_library_depends(${BINDIR}/rocm-ernic "${needed}")

if(BUNDLE_VFIO_USER)
    set(private_lib_dir ${LIBDIR}/rocm-ernic)
    cmake_path(RELATIVE_PATH private_lib_dir BASE_DIRECTORY ${BINDIR}
        OUTPUT_VARIABLE relative_rpath)
    set(expected_rpath "$ORIGIN/${relative_rpath}")
    if(NOT rpath STREQUAL expected_rpath OR NOT runpath STREQUAL "")
        ernic_fail("${BINDIR}/rocm-ernic has DT_RPATH '${rpath}' and "
            "DT_RUNPATH '${runpath}', not DT_RPATH '${expected_rpath}' and "
            "no DT_RUNPATH")
    endif()
    if(NOT VFIO_USER_SONAME IN_LIST needed)
        ernic_fail("${BINDIR}/rocm-ernic doesn't need ${VFIO_USER_SONAME}")
    endif()

    ernic_read_dynamic(needed rpath runpath ${root}${private_vfio_user})
    if(NOT rpath STREQUAL "" OR NOT runpath STREQUAL "")
        ernic_fail("${private_vfio_user} has a run path (DT_RPATH '${rpath}', "
            "DT_RUNPATH '${runpath}')")
    endif()
    ernic_check_library_depends(${private_vfio_user} "${needed}")
endif()

# ── Paths in the installed files ─────────────────

# The ELF files are left out: what the compiler and the libraries put in
# them isn't up to the install rules
file(GLOB_RECURSE installed_files LIST_DIRECTORIES false ${root}/*)
set(text_files)
foreach(file IN LISTS installed_files)
    file(READ ${file} magic LIMIT 4 HEX)
    if(NOT magic STREQUAL "7f454c46")
        list(APPEND text_files ${file})
    endif()
endforeach()

# Where the installed files should read the configuration. Under the
# prefix, the packaged file is a copy to put in /etc/rocm-ernic, so they
# read it there; outside it, as in /etc or /etc/opt/<name>, they read
# the packaged file.
set(read_env_file)
if(INSTALL_SERVICE)
    cmake_path(IS_PREFIX INSTALL_PREFIX "${ENV_FILE}" NORMALIZE
        env_file_in_prefix)
    if(env_file_in_prefix)
        set(read_env_file /etc/rocm-ernic/rocm-ernic.env)
    else()
        set(read_env_file "${ENV_FILE}")
    endif()
endif()

# No file names /usr/local/ unless that's the prefix. The configuration
# can be outside the prefix in a directory under /usr/local, so the path
# the files read it from doesn't count.
if(NOT INSTALL_PREFIX STREQUAL "/usr/local")
    foreach(file IN LISTS text_files)
        file(STRINGS ${file} lines REGEX "/usr/local/")
        foreach(line IN LISTS lines)
            if(read_env_file)
                string(REPLACE "${read_env_file}" "" rest "${line}")
            else()
                set(rest "${line}")
            endif()
            if(rest MATCHES "/usr/local/")
                string(REPLACE "${root}" "" path "${file}")
                ernic_fail("${path} names /usr/local/, but the packages "
                    "install to ${INSTALL_PREFIX}: ${line}")
            endif()
        endforeach()
    endforeach()
endif()

# Every file that names a rocm-ernic.env names that one
foreach(file IN LISTS text_files)
    file(STRINGS ${file} lines REGEX "/rocm-ernic\\.env")
    foreach(line IN LISTS lines)
        string(REGEX MATCHALL "/[^ \t\"'`=]*/rocm-ernic\\.env" names
            "${line}")
        foreach(name IN LISTS names)
            if(NOT name STREQUAL read_env_file)
                string(REPLACE "${root}" "" path "${file}")
                ernic_fail("${path} names ${name}, not ${read_env_file}: "
                    "${line}")
            endif()
        endforeach()
    endforeach()
endforeach()

# Every program a unit runs from under the prefix is in a package, and
# each unit reads that configuration, and nothing else. An Exec line's
# command can start with characters that change how systemd runs it,
# such as "-", which are removed.
file(GLOB units ${root}${units_dir}/*.service)
foreach(unit IN LISTS units)
    string(REPLACE "${root}" "" unit_path "${unit}")
    file(STRINGS ${unit} lines REGEX "^(Exec[A-Za-z]*|EnvironmentFile)=")
    set(reads_env_file OFF)
    foreach(line IN LISTS lines)
        if(line MATCHES "^EnvironmentFile=-?(.*)$")
            set(env_file "${CMAKE_MATCH_1}")
            if(NOT env_file STREQUAL read_env_file)
                ernic_fail("${unit_path} reads ${env_file}, not "
                    "${read_env_file}")
            endif()
            set(reads_env_file ON)
            continue()
        endif()
        string(REGEX REPLACE "^Exec[A-Za-z]*=[-@:+!]*" "" command "${line}")
        separate_arguments(words UNIX_COMMAND "${command}")
        foreach(word IN LISTS words)
            string(FIND "${word}" "${INSTALL_PREFIX}/" position)
            if(position EQUAL 0 AND NOT word IN_LIST all_files)
                ernic_fail("${unit_path} runs ${word}, which isn't in a "
                    "package")
            endif()
        endforeach()
    endforeach()
    if(NOT reads_env_file)
        ernic_fail("${unit_path} doesn't read ${read_env_file} "
            "(no EnvironmentFile=)")
    endif()
endforeach()
if(INSTALL_SERVICE AND NOT units)
    ernic_fail("found no systemd units in the packages")
endif()

message(STATUS "run-package-test: OK")
