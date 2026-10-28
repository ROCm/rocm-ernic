# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Check that every C file is compiled against the same language and C
# library interface (run by the ernic-feature-macros test; see
# tests/CMakeLists.txt)
#
# -std=c11 fixes the language, but the C library's headers declare what the
# feature-test macros (_GNU_SOURCE, _ISOC23_SOURCE, _XOPEN_SOURCE, ...) ask
# for. One defined in a source file, a header it includes, or the compiler
# flags would give that file a different library: _GNU_SOURCE, for one,
# also turns on glibc's C23 interfaces. Only src/ernic_gnu.c may define it.
#
# For each of the project's entries in compile_commands.json, this
# preprocesses the source with -dM, which lists every macro defined at its
# end, and compares the feature macros, and the macros glibc derives from
# them (__USE_*, __GLIBC_USE_*), with those of a probe file compiled with
# the same flags: one that only includes <stdio.h> for a file that includes
# a C library header, which lets glibc's <features.h> rewrite them, or an
# empty one for a file that doesn't. A file may not define a feature macro
# the probe doesn't, or give one a different value, and must have every
# feature macro and __USE_* macro the probe has. (Some headers define
# __USE_* or __GLIBC_USE_* macros of their own, so a file can have macros
# the probe doesn't, and the other way around.) The <stdio.h> probe itself
# is held to the standard and the macros ernic_set_compiler_flags()
# defines. Then, for the
# files this build doesn't compile (the guest-side test programs, or P4
# with ERNIC_BUILD_P4 off), it looks for a #define or #undef of a feature
# macro in the sources.
#
# Variables:
#   COMPILE_COMMANDS  The compile_commands.json to check
#   SOURCE_DIR        The project's source directory
#   BINARY_DIR        The project's build directory; only the entries
#                     whose output is under it are checked
#   GNU_SOURCE_FILE   The one file allowed to define _GNU_SOURCE
#   STDC_VERSION      The __STDC_VERSION__ every file must have

cmake_minimum_required(VERSION 3.21)

foreach(var IN ITEMS COMPILE_COMMANDS SOURCE_DIR BINARY_DIR GNU_SOURCE_FILE
        STDC_VERSION)
    if(NOT ${var})
        message(FATAL_ERROR "ErnicCheckFeatureMacros: ${var} is not set")
    endif()
endforeach()

# The macros that choose, or record, which interfaces the C library
# declares
set(feature_macro
  "_[A-Za-z0-9_]*_SOURCE|_FILE_OFFSET_BITS|_TIME_BITS|_REENTRANT|_THREAD_SAFE|__STRICT_ANSI__")
set(feature_state "${feature_macro}|__USE_[A-Za-z0-9_]+|__GLIBC_USE_[A-Za-z0-9_]+|__STDC_VERSION__")

# The feature macros a file may end up with: what ernic_set_compiler_flags()
# defines, what glibc's <features.h> defines from it, and _FORTIFY_SOURCE
# (ErnicFortifySource.cmake). glibc raises _POSIX_C_SOURCE to its own
# default under _DEFAULT_SOURCE, so its value isn't checked.
set(allowed_sources
  _ATFILE_SOURCE _DEFAULT_SOURCE _FORTIFY_SOURCE _POSIX_C_SOURCE _POSIX_SOURCE)

set(failures 0)
set(only_gnu
  "Only src/ernic_gnu.c may define _GNU_SOURCE, see src/ernic_gnu.h.")

# ernic_fm_preprocess(<outvar> <has_libc_var> <dir> <arg>...)
#
# Preprocess with the compiler command <arg>... in <dir> and set <outvar>
# to the list of feature macro definitions at the end, each
# "#define <name> <value>", and <has_libc_var> to whether glibc's
# <features.h> was included
function(ernic_fm_preprocess outvar has_libc_var dir)
    execute_process(
      COMMAND ${ARGN} -E -dM
      WORKING_DIRECTORY "${dir}"
      OUTPUT_VARIABLE out
      ERROR_VARIABLE err
      RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        list(JOIN ARGN " " cmd)
        message(FATAL_ERROR "Preprocessing failed (${rc}): ${cmd}\n${err}")
    endif()
    string(REGEX MATCHALL "#define (${feature_state})( [^\n]*)?\n" defs
      "${out}")
    list(TRANSFORM defs STRIP)
    set(${outvar} "${defs}" PARENT_SCOPE)
    if(out MATCHES "#define _FEATURES_H ")
        set(${has_libc_var} TRUE PARENT_SCOPE)
    else()
        set(${has_libc_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

# ernic_fm_fail(<what> <message>...)
#
# Report a failure in <what>; the <message> strings are concatenated
macro(ernic_fm_fail what)
    message(SEND_ERROR "${what}: " ${ARGN})
    math(EXPR failures "${failures} + 1")
endmacro()

# ernic_fm_check_language(<what> <defs>)
#
# The language checks every file must pass: the standard, and no GNU
# dialect
macro(ernic_fm_check_language what defs)
    if(NOT "#define __STDC_VERSION__ ${STDC_VERSION}" IN_LIST ${defs})
        ernic_fm_fail("${what}"
          "not compiled as __STDC_VERSION__ ${STDC_VERSION}")
    endif()
    if(NOT "#define __STRICT_ANSI__ 1" IN_LIST ${defs})
        ernic_fm_fail("${what}"
          "compiled with GNU extensions to the language (__STRICT_ANSI__ "
          "isn't defined)")
    endif()
endmacro()

# -- Every file this build compiles -----------------------------------

file(READ "${COMPILE_COMMANDS}" json)
string(JSON n_entries LENGTH "${json}")
math(EXPR last "${n_entries} - 1")

set(probe "${BINARY_DIR}/CMakeFiles/ernic-feature-macros-probe.c")
file(WRITE "${probe}" "#include <stdio.h>\n")
set(bare_probe "${BINARY_DIR}/CMakeFiles/ernic-feature-macros-bare.c")
file(WRITE "${bare_probe}" "typedef int ernic_feature_macros_bare;\n")

set(n_checked 0)
foreach(i RANGE ${last})
    string(JSON entry GET "${json}" ${i})
    string(JSON output GET "${entry}" output)
    string(JSON dir GET "${entry}" directory)
    if(NOT IS_ABSOLUTE "${output}")
        set(output "${dir}/${output}")
    endif()
    cmake_path(IS_PREFIX BINARY_DIR "${output}" NORMALIZE ours)
    if(NOT ours)
        continue()
    endif()

    # The command without its outputs and source, so the source can be
    # swapped for the probe. -dM goes to stdout, and the dependency file
    # options would overwrite the build's.
    string(JSON command GET "${entry}" command)
    separate_arguments(args UNIX_COMMAND "${command}")
    set(flags)
    set(source)
    set(skip_next FALSE)
    set(source_next FALSE)
    foreach(arg IN LISTS args)
        if(skip_next)
            set(skip_next FALSE)
        elseif(source_next)
            set(source "${arg}")
            set(source_next FALSE)
        elseif(arg STREQUAL "-c")
            set(source_next TRUE)
        elseif(arg MATCHES "^-(o|MF|MT|MQ)$")
            set(skip_next TRUE)
        elseif(NOT arg MATCHES "^-(MD|MMD)$")
            list(APPEND flags "${arg}")
        endif()
    endforeach()
    if(NOT source)
        message(FATAL_ERROR "No -c <source> in: ${command}")
    endif()
    cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${dir}" NORMALIZE
      OUTPUT_VARIABLE source_path)
    cmake_path(RELATIVE_PATH source_path BASE_DIRECTORY "${SOURCE_DIR}"
      OUTPUT_VARIABLE what)

    # The probes, once for each set of flags
    string(MD5 key "${flags}")
    if(NOT DEFINED probe_${key})
        ernic_fm_preprocess(probe_defs probe_libc "${dir}" ${flags}
          "${probe}")
        set(probe_${key} "${probe_defs}")
        ernic_fm_preprocess(bare_defs bare_libc "${dir}" ${flags}
          "${bare_probe}")
        set(bare_${key} "${bare_defs}")
        ernic_fm_check_language("Compiler flags for ${what}" probe_defs)
        foreach(def IN LISTS probe_defs)
            if(def MATCHES "^#define (_[A-Za-z0-9_]*_SOURCE)( |$)"
                AND NOT CMAKE_MATCH_1 IN_LIST allowed_sources)
                ernic_fm_fail("Compiler flags for ${what}"
                  "define ${CMAKE_MATCH_1}. ${only_gnu}")
            elseif(def MATCHES "^#define __USE_GNU( |$)")
                ernic_fm_fail("Compiler flags for ${what}"
                  "turn on the GNU extensions (__USE_GNU)")
            endif()
        endforeach()
    endif()

    ernic_fm_preprocess(defs has_libc "${dir}" ${flags} "${source}")
    math(EXPR n_checked "${n_checked} + 1")
    ernic_fm_check_language("${what}" defs)
    if(source_path STREQUAL GNU_SOURCE_FILE)
        continue()
    endif()

    if(has_libc)
        set(expected "${probe_${key}}")
    else()
        set(expected "${bare_${key}}")
    endif()
    set(expected_names)
    foreach(def IN LISTS expected)
        string(REGEX MATCH "^#define ([^ ]+) ?(.*)$" match "${def}")
        list(APPEND expected_names ${CMAKE_MATCH_1})
        set(expected_value_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endforeach()
    set(names)
    foreach(def IN LISTS defs)
        string(REGEX MATCH "^#define ([^ ]+) ?(.*)$" match "${def}")
        set(name ${CMAKE_MATCH_1})
        set(value "${CMAKE_MATCH_2}")
        list(APPEND names ${name})
        if(name IN_LIST expected_names)
            if(NOT value STREQUAL expected_value_${name})
                ernic_fm_fail("${what}" "${name} is ${value}, not "
                  "${expected_value_${name}} as its compiler flags make it. "
                  "${only_gnu}")
            endif()
        elseif(name MATCHES "^(${feature_macro}|__USE_GNU)$")
            ernic_fm_fail("${what}" "defines ${name}. ${only_gnu}")
        endif()
    endforeach()
    foreach(name IN LISTS expected_names)
        if(name MATCHES "^(${feature_macro}|__USE_[A-Za-z0-9_]+)$"
            AND NOT name IN_LIST names)
            ernic_fm_fail("${what}" "doesn't define ${name}, which its "
              "compiler flags do")
        endif()
    endforeach()
endforeach()
file(REMOVE "${probe}" "${bare_probe}")

if(n_checked EQUAL 0)
    message(FATAL_ERROR "No compile commands under ${BINARY_DIR} in "
      "${COMPILE_COMMANDS}")
endif()

# -- Every source file, compiled here or not --------------------------

set(sources)
foreach(subdir IN ITEMS src tests shared cmake nix)
    file(GLOB_RECURSE found "${SOURCE_DIR}/${subdir}/*.c"
      "${SOURCE_DIR}/${subdir}/*.h")
    list(APPEND sources ${found})
endforeach()
list(REMOVE_ITEM sources "${GNU_SOURCE_FILE}")
foreach(source_file IN LISTS sources)
    file(STRINGS "${source_file}" lines REGEX
      "^[ \t]*#[ \t]*(define|undef)[ \t]+(${feature_macro})([ \t(]|$)")
    foreach(line IN LISTS lines)
        cmake_path(RELATIVE_PATH source_file BASE_DIRECTORY "${SOURCE_DIR}"
          OUTPUT_VARIABLE what)
        string(STRIP "${line}" line)
        ernic_fm_fail("${what}" "\"${line}\". ${only_gnu}")
    endforeach()
endforeach()

list(LENGTH sources n_sources)
if(failures GREATER 0)
    message(FATAL_ERROR "${failures} feature macro problem(s)")
endif()
message(STATUS "${n_checked} compiles and ${n_sources} sources use only "
  "the project's language and C library interface")
