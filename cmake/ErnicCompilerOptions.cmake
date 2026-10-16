# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Set compiler and linker flags on target based on the compiler in use.
#
# Follows the pattern from hipFile AISCompilerOptions.cmake and
# rocm-xio XIOCompilerOptions.cmake, adapted for C-only builds.

include_guard(GLOBAL)

include(CheckLinkerFlag)
include(CheckPIESupported)
include(ErnicClangCompilerOptions)
include(ErnicGNUCompilerOptions)
include(ErnicSanitizers)
include(ErnicCoverage)
include(ErnicIWYU)
include(ErnicClangTidy)

set(ERNIC_VENDORED_DIR "${PROJECT_SOURCE_DIR}/third-party")

# Linker hardening, following the OpenSSF Compiler Options Hardening Guide
# for C and C++. Some distributions' compilers pass some of these by
# default (Ubuntu's GCC passes all but --no-copy-dt-needed-entries, which
# GNU ld and lld already default to), but upstream Clang, which TheRock
# uses, doesn't, so they're passed explicitly.
#
# https://best.openssf.org/Compiler-Hardening-Guides/Compiler-Options-Hardening-Guide-for-C-and-C++.html
check_linker_flag(C "-Wl,-z,noexecstack"
  ERNIC_LINKER_SUPPORTS_NOEXECSTACK)
check_linker_flag(C "-Wl,-z,relro" ERNIC_LINKER_SUPPORTS_RELRO)
check_linker_flag(C "-Wl,-z,now" ERNIC_LINKER_SUPPORTS_NOW)
check_linker_flag(C "-Wl,--as-needed" ERNIC_LINKER_SUPPORTS_AS_NEEDED)
check_linker_flag(C "-Wl,--no-copy-dt-needed-entries"
  ERNIC_LINKER_SUPPORTS_NO_COPY_DT_NEEDED)

# Without this, POSITION_INDEPENDENT_CODE only adds -fPIE to the compile,
# not -pie to the link, and the executable is only a PIE if the compiler
# links one by default
check_pie_supported(OUTPUT_VARIABLE ernic_pie_output LANGUAGES C)
if(NOT CMAKE_C_LINK_PIE_SUPPORTED)
    message(WARNING
      "The C linker can't build position-independent executables, so "
      "rocm-ernic won't be one:\n${ernic_pie_output}")
endif()

# Apply linker hardening flags to ``target``
#
# NOTE: CMake ignores link options on static and object libraries,
#       so these only take effect on executables and shared libraries.
function(ernic_set_linker_flags target)
    # Build a position-independent executable, so ASLR can move the
    # program itself and not just its libraries, stack, and heap
    if(CMAKE_C_LINK_PIE_SUPPORTED)
        set_target_properties(${target} PROPERTIES
          POSITION_INDEPENDENT_CODE ON)
    endif()

    if(ERNIC_LINKER_SUPPORTS_NOEXECSTACK)
        target_link_options(${target} PRIVATE "-Wl,-z,noexecstack")
    endif()

    # Make the GOT and the other data that only the dynamic linker writes
    # read-only once it has finished relocating (RELRO), and resolve every
    # symbol at load time, so RELRO covers the whole GOT (full RELRO)
    if(ERNIC_LINKER_SUPPORTS_RELRO)
        target_link_options(${target} PRIVATE "-Wl,-z,relro")
    endif()
    if(ERNIC_LINKER_SUPPORTS_NOW)
        target_link_options(${target} PRIVATE "-Wl,-z,now")
    endif()

    # Only record the shared libraries the target uses directly, so it
    # doesn't load ones it never calls
    #
    # NOTE: --as-needed only affects the libraries after it on the link
    #       line. CMake puts link options before the objects and libraries,
    #       so it covers all of them.
    if(ERNIC_LINKER_SUPPORTS_AS_NEEDED)
        target_link_options(${target} PRIVATE "-Wl,--as-needed")
    endif()
    if(ERNIC_LINKER_SUPPORTS_NO_COPY_DT_NEEDED)
        target_link_options(${target} PRIVATE
          "-Wl,--no-copy-dt-needed-entries")
    endif()
endfunction()

# Apply compiler warning and hardening flags to ``target``
# based on the compiler (GCC or Clang) used for each source,
# along with the linker hardening flags.
function(ernic_set_compiler_flags target)
    get_target_property(sources ${target} SOURCES)
    foreach(source IN LISTS sources)
        unset(compiler_flags)
        get_source_file_property(language ${source} LANGUAGE)
        if(NOT language)
            set(language C)
        endif()
        set(compiler_id "${CMAKE_${language}_COMPILER_ID}")
        set(compiler_version
          "${CMAKE_${language}_COMPILER_VERSION}")

        if(compiler_id STREQUAL "GNU")
            get_ernic_gnu_warning_flags(compiler_flags
              ${compiler_version})
        elseif(compiler_id STREQUAL "Clang")
            get_ernic_clang_warning_flags(compiler_flags
              ${compiler_version})
        endif()

        target_compile_options(${target} PRIVATE
          $<$<COMPILE_LANG_AND_ID:${language},${compiler_id}>:${compiler_flags}>)
    endforeach()

    ernic_set_linker_flags(${target})

    ernic_add_sanitizers(${target})

    if(ERNIC_USE_CODE_COVERAGE)
        ernic_add_coverage(${target})
    endif()

    ernic_set_iwyu(${target})
    ernic_set_clang_tidy(${target})

    if(ERNIC_USE_IWYU OR ERNIC_USE_CLANG_TIDY)
        ernic_skip_linting_vendored_sources(${target})
    endif()
endfunction()

# Keep the linters (IWYU and clang-tidy) off a target's vendored sources
# under third-party/, which aren't ours to fix
#
# Source file properties belong to a directory, so call this from the
# directory that creates the target.
function(ernic_skip_linting_vendored_sources target)
    get_target_property(sources ${target} SOURCES)
    get_target_property(source_dir ${target} SOURCE_DIR)
    foreach(source IN LISTS sources)
        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${source_dir}"
          OUTPUT_VARIABLE path)
        cmake_path(IS_PREFIX ERNIC_VENDORED_DIR "${path}" NORMALIZE
          vendored)
        if(vendored)
            set_source_files_properties(${source} PROPERTIES
              SKIP_LINTING ON)
        endif()
    endforeach()
endfunction()
