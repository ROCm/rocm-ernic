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

# Off by default so a downstream or distro build is never broken by a
# warning from a compiler version we have not seen. CI's build jobs turn
# it on, so new warnings still fail the build there.
option(ERNIC_WERROR
  "Treat compiler warnings as errors for project code"
  OFF)

# Fill every automatic variable the code doesn't initialize with a
# pattern, so a read of one before it's written can't see stale stack
# memory. Turn it off for a build that runs under valgrind's memcheck,
# since initializing every variable hides the reads memcheck reports.
option(ERNIC_TRIVIAL_AUTO_VAR_INIT
  "Initialize automatic variables with a pattern (off for valgrind)"
  ON)

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
# based on the C compiler (GCC or Clang), along with the
# feature-test macros, the linker hardening flags, and -Werror
# if ERNIC_WERROR is on.
function(ernic_set_compiler_flags target)
    # The library interfaces the code may use: C11 (CMAKE_C_STANDARD)
    # plus POSIX.1-2008, and glibc's _DEFAULT_SOURCE for the BSD and
    # System V extensions it relies on (libvfio-user's public header uses
    # loff_t, and src/p4/p4_devcmd.c uses <endian.h>'s htole32() family).
    # glibc defines both by default, but not under -std=c11.
    #
    # Not _GNU_SOURCE, which makes every GNU extension visible, so a
    # GNU-only function could slip in unnoticed, and which hides
    # clang-tidy's android-cloexec-* findings. A source file that needs a
    # GNU extension defines _GNU_SOURCE itself, before its first
    # #include, and says what for.
    #
    # NOTE: ernic_check_libc_disabled_macro_expansion() compiles its check
    #       with the same macros; change both together.
    target_compile_definitions(${target} PRIVATE
      _POSIX_C_SOURCE=200809L
      _DEFAULT_SOURCE
    )

    # Every source is C, the only language project() enables. The flags
    # apply to the whole target, including sources added after this call.
    set(compiler_flags)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        ernic_get_gnu_warning_flags(compiler_flags
          ${CMAKE_C_COMPILER_VERSION})
    elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        ernic_get_clang_warning_flags(compiler_flags
          ${CMAKE_C_COMPILER_VERSION})
    endif()
    if(compiler_flags)
        target_compile_options(${target} PRIVATE ${compiler_flags})
    endif()

    if(ERNIC_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()

    ernic_set_linker_flags(${target})

    ernic_add_sanitizers(${target})

    if(ERNIC_USE_CODE_COVERAGE)
        ernic_add_coverage(${target})
    endif()

    ernic_set_iwyu(${target})
    ernic_set_clang_tidy(${target})
endfunction()
