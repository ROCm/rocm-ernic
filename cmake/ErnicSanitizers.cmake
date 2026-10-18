# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Sanitizer support for rocm-ernic
#
# There are two options:
#
#   - ERNIC_USE_SANITIZERS enables every sanitizer, except
#     ThreadSanitizer, that the compiler supports and that can be combined
#     with AddressSanitizer. The lists for GCC and Clang are below.
#   - ERNIC_USE_THREAD_SANITIZER enables ThreadSanitizer.
#
# ThreadSanitizer can't be combined with AddressSanitizer, so only one of
# the options can be on. Both options work with GCC and with Clang.
#
# ERNIC_SANITIZER_SHARED_RUNTIME selects how programs get the sanitizer
# runtime. When it's on, they link the runtime's shared library.
# Libraries from another sanitizer build that uses the shared runtime,
# such as a sanitizer build of libvfio-user, can then be loaded into the
# same program. When it's off, programs link the static runtime instead.
# It defaults to what the compiler does without an option: on with GCC,
# and off with Clang. Ubuntu's Clang 18, which CI uses, builds its shared
# runtimes so that they never call a program's __asan_default_options()
# or __tsan_default_options(), so with them the defaults in
# sanitizer-default-options.c are ignored.
#
# Libraries that come from the system, such as libvfio-user, GLib, and
# libibverbs, aren't instrumented.
#
# The options are checked, and the compiler is tested with the sanitizer
# flags, when this module is included.

include_guard(GLOBAL)

include(CheckSourceCompiles)

option(ERNIC_USE_SANITIZERS
  "Build with every sanitizer except ThreadSanitizer"
  OFF)
option(ERNIC_USE_THREAD_SANITIZER
  "Build with ThreadSanitizer (not with ERNIC_USE_SANITIZERS)"
  OFF)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(ernic_shared_runtime_default ON)
else()
    set(ernic_shared_runtime_default OFF)
endif()
option(ERNIC_SANITIZER_SHARED_RUNTIME
  "Link the shared sanitizer runtime instead of the static one"
  ${ernic_shared_runtime_default})

# Add the compile and link flags for ERNIC_USE_SANITIZERS or
# ERNIC_USE_THREAD_SANITIZER, whichever is on, to ``target``, and, if it's
# a program, the sanitizers' default run-time options (see
# sanitizer-default-options.c). Does nothing if neither is on.
# ernic_set_compiler_flags() calls this for every target.
function(ernic_add_sanitizers target)
    get_property(compile_options GLOBAL
      PROPERTY ERNIC_SANITIZER_COMPILE_OPTIONS)
    get_property(link_options GLOBAL
      PROPERTY ERNIC_SANITIZER_LINK_OPTIONS)
    if(NOT compile_options)
        return()
    endif()

    target_compile_options(${target} PRIVATE ${compile_options})
    target_link_options(${target} PRIVATE ${link_options})

    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
        set(defaults
          ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sanitizer-default-options.c)
        target_sources(${target} PRIVATE ${defaults})
        # Only this file needs the warning suppression macros, so only it
        # gets their directory
        cmake_path(SET shared_dir NORMALIZE
          "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../shared")
        set_property(SOURCE ${defaults} TARGET_DIRECTORY ${target}
          APPEND PROPERTY INCLUDE_DIRECTORIES ${shared_dir})
    endif()
endfunction()

# Build ``target`` without AddressSanitizer's use-after-return
# instrumentation, so that its functions' frames are always on the real
# stack. The run-time option detect_stack_use_after_return can't do this
# reliably: the ASAN_OPTIONS environment variable overrides it, and
# Ubuntu's Clang 18 shared runtime ignores a program's default options.
# Does nothing unless ERNIC_USE_SANITIZERS is on, since AddressSanitizer
# is off otherwise.
function(ernic_disable_asan_use_after_return target)
    if(NOT ERNIC_USE_SANITIZERS)
        return()
    endif()
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${target} PRIVATE
          -fsanitize-address-use-after-return=never)
    else()
        target_compile_options(${target} PRIVATE
          --param=asan-use-after-return=0)
    endif()
endfunction()

# Find the directory that holds a sanitizer's shared runtime library, and
# store it in out_var if programs have to be told where it is (an RPATH).
# out_var is empty if the dynamic linker already searches the directory.
function(ernic_find_sanitizer_runtime_dir out_var runtime)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        # Clang's runtimes are named for the architecture, except when
        # it's built with a separate runtime directory for each target
        set(names
          libclang_rt.${runtime}-${CMAKE_SYSTEM_PROCESSOR}.so
          libclang_rt.${runtime}.so)
    else()
        set(names lib${runtime}.so)
    endif()

    # The compiler prints the name it was given if it can't find the
    # file. It's given the user's flags, since some, such as
    # -resource-dir, change where it looks.
    separate_arguments(flags NATIVE_COMMAND "${CMAKE_C_FLAGS}")
    foreach(name IN LISTS names)
        execute_process(
          COMMAND ${CMAKE_C_COMPILER} ${flags} -print-file-name=${name}
          OUTPUT_VARIABLE path
          OUTPUT_STRIP_TRAILING_WHITESPACE
          RESULT_VARIABLE result)
        if(result EQUAL 0 AND IS_ABSOLUTE "${path}" AND EXISTS "${path}")
            break()
        endif()
        set(path)
    endforeach()
    if(NOT path)
        list(JOIN names ", " names)
        message(FATAL_ERROR
          "Can't find the shared runtime library for the sanitizers. "
          "${CMAKE_C_COMPILER} can't find any of these: ${names}. Install "
          "the compiler's sanitizer runtimes, or set "
          "ERNIC_SANITIZER_SHARED_RUNTIME to OFF.")
    endif()

    # GCC's libasan.so, for example, is a link to the libasan.so.8 in a
    # system library directory, which the dynamic linker searches already.
    # An RPATH to a system directory would put it ahead of the program's
    # other RPATH entries, such as libvfio-user's when it's installed
    # outside the system directories, so it's left out.
    #
    # The system directories are CMake's list for the platform, plus the
    # multiarch directories, such as Debian's /usr/lib/x86_64-linux-gnu.
    # CMAKE_C_IMPLICIT_LINK_DIRECTORIES isn't used, since it also has the
    # compiler's own directories, such as GCC's
    # /usr/lib/gcc/x86_64-linux-gnu/<version>, which the linker searches
    # but the dynamic linker doesn't.
    file(REAL_PATH "${path}" path)
    cmake_path(GET path PARENT_PATH dir)
    set(system_dirs ${CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES})
    if(CMAKE_LIBRARY_ARCHITECTURE)
        list(APPEND system_dirs
          /lib/${CMAKE_LIBRARY_ARCHITECTURE}
          /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE})
    endif()
    set(${out_var} PARENT_SCOPE)
    foreach(system_dir IN LISTS system_dirs)
        file(REAL_PATH "${system_dir}" system_dir)
        if(dir STREQUAL system_dir)
            return()
        endif()
    endforeach()
    set(${out_var} ${dir} PARENT_SCOPE)
endfunction()

# Store the compile and link flags for the sanitizers in ``checks`` (a
# comma-separated -fsanitize list), whose runtime library is ``runtime``,
# in compile_var and link_var. Fails if the C compiler can't build a
# program with them. ``reason`` says what needs them, for the error.
function(ernic_get_sanitizer_flags compile_var link_var checks runtime
    reason)
    # -fno-sanitize-recover=all makes AddressSanitizer's and
    # UndefinedBehaviorSanitizer's reports fatal, so a test with a report
    # fails. Without it, UndefinedBehaviorSanitizer prints the report and
    # the program keeps running. It doesn't affect ThreadSanitizer, which
    # a run-time option makes stop instead (see
    # sanitizer-default-options.c).
    set(options
      -fsanitize=${checks}
      -fno-sanitize-recover=all)
    set(compile_options ${options} -fno-omit-frame-pointer)
    set(link_options ${options})

    # Select the shared or the static runtime. A program that uses the
    # shared runtime has to find it when it runs, so add its directory to
    # the program's RPATH if the dynamic linker doesn't search it already.
    if(ERNIC_SANITIZER_SHARED_RUNTIME)
        if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
            list(APPEND link_options -shared-libsan)
        endif()
        ernic_find_sanitizer_runtime_dir(runtime_dir ${runtime})
        if(runtime_dir)
            list(APPEND link_options "-Wl,-rpath,${runtime_dir}")
        endif()
    elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        list(APPEND link_options -static-libsan)
    elseif(runtime STREQUAL "tsan")
        list(APPEND link_options -static-libtsan)
    else()
        list(APPEND link_options -static-libasan -static-libubsan)
    endif()

    # Check that the compiler can build and link a program with the
    # flags. This fails if, for example, the sanitizer runtimes aren't
    # installed. The result is cached under a name made from the flags,
    # so it's checked again if they change.
    string(MD5 hash "${compile_options};${link_options}")
    list(JOIN compile_options " " CMAKE_REQUIRED_FLAGS)
    set(CMAKE_REQUIRED_LINK_OPTIONS ${link_options})
    set(CMAKE_REQUIRED_QUIET ON)
    check_source_compiles(C "int main(void) { return 0; }"
      ERNIC_SANITIZERS_WORK_${hash})
    if(NOT ERNIC_SANITIZERS_WORK_${hash})
        list(JOIN link_options " " link_flags)
        message(FATAL_ERROR
          "The C compiler, ${CMAKE_C_COMPILER}, can't build a program "
          "with the sanitizers, which ${reason}. Check that its sanitizer "
          "runtimes are installed. The flags were:\n"
          "  Compile: ${CMAKE_REQUIRED_FLAGS}\n"
          "  Link: ${link_flags}")
    endif()

    set(${compile_var} ${compile_options} PARENT_SCOPE)
    set(${link_var} ${link_options} PARENT_SCOPE)
endfunction()

if(ERNIC_USE_SANITIZERS AND ERNIC_USE_THREAD_SANITIZER)
    message(FATAL_ERROR
      "ERNIC_USE_SANITIZERS and ERNIC_USE_THREAD_SANITIZER can't both be "
      "on, since ThreadSanitizer can't be combined with AddressSanitizer")
endif()

if(ERNIC_USE_THREAD_SANITIZER)
    message(WARNING
      "TSAN has known problems with higher levels of entropy, "
      "try using `sudo sysctl vm.mmap_rnd_bits=28` if you "
      "encounter errors concerning unexpected memory mappings.")
endif()

if(NOT (ERNIC_USE_SANITIZERS OR ERNIC_USE_THREAD_SANITIZER))
    return()
endif()

if(NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang)$")
    message(FATAL_ERROR
      "The sanitizers need GCC or Clang as the C compiler, but it's "
      "${CMAKE_C_COMPILER_ID}")
endif()

# The -fsanitize groups for ERNIC_USE_SANITIZERS
#
# Both compilers get:
#
#   - address: AddressSanitizer. On Linux, this includes LeakSanitizer.
#   - undefined: UndefinedBehaviorSanitizer's default checks.
#   - float-divide-by-zero: floating-point division by zero, which
#     "undefined" leaves out.
#
# Clang also gets these checks, which its "undefined" leaves out:
#
#   - implicit-conversion: implicit conversions that change a value.
#     These are defined behavior, but they're usually bugs.
#   - local-bounds: out-of-bounds accesses that can be found from local
#     information alone.
#   - nullability: violations of the _Nonnull annotations.
#
# Clang's "integer" group isn't used. "implicit-conversion" includes its
# conversion checks, and "undefined" includes its checks for signed
# overflow, shifts, and division by zero. Its remaining checks are for
# unsigned overflow, which is defined behavior that code such as
# checksums and ring indices depends on.
#
# GCC also gets these checks:
#
#   - bounds-strict: the "bounds" check, extended to arrays at the end
#     of a struct.
#   - float-cast-overflow: conversions from floating point to an integer
#     type that can't hold the value. Clang's "undefined" includes it.
#   - pointer-compare and pointer-subtract: AddressSanitizer checks for
#     comparing or subtracting pointers into different objects. They only
#     run when the run-time option detect_invalid_pointer_pairs is set,
#     which sanitizer-default-options.c does.
#
# Clang doesn't get pointer-compare and pointer-subtract. It instruments
# comparisons and subtractions after it optimizes, so at -O1 and above it
# reports ones that the optimizer made: comparisons of pointers to
# constants that its pointer-overflow check (part of "undefined") adds,
# and subtractions that it moves ahead of the null check that guards
# them. GCC instruments them before it optimizes.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set(ernic_asan_checks
      address
      undefined
      float-divide-by-zero
      implicit-conversion
      local-bounds
      nullability)
else()
    set(ernic_asan_checks
      address
      undefined
      float-divide-by-zero
      bounds-strict
      float-cast-overflow
      pointer-compare
      pointer-subtract)
endif()
list(JOIN ernic_asan_checks "," ernic_asan_checks)

if(ERNIC_USE_SANITIZERS)
    ernic_get_sanitizer_flags(ernic_sanitizer_compile_options
      ernic_sanitizer_link_options ${ernic_asan_checks} asan
      "ERNIC_USE_SANITIZERS turns on")
    message(STATUS "Sanitizers: ${ernic_asan_checks}")
else()
    ernic_get_sanitizer_flags(ernic_sanitizer_compile_options
      ernic_sanitizer_link_options thread tsan
      "ERNIC_USE_THREAD_SANITIZER turns on")
    message(STATUS "Sanitizers: thread")
endif()

set_property(GLOBAL PROPERTY ERNIC_SANITIZER_COMPILE_OPTIONS
  ${ernic_sanitizer_compile_options})
set_property(GLOBAL PROPERTY ERNIC_SANITIZER_LINK_OPTIONS
  ${ernic_sanitizer_link_options})
