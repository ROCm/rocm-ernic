# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Sanitizer options for rocm-ernic.
#
# Follows the pattern from hipFile AISSanitizers.cmake and
# rocm-xio XIOSanitizers.cmake.

option(ERNIC_USE_SANITIZERS
  "Build with -fsanitize=address, leak, and undefined" OFF)
option(ERNIC_USE_THREAD_SANITIZER
  "Build with -fsanitize=thread (incompatible with ERNIC_USE_SANITIZERS)"
  OFF)
option(ERNIC_TEST_DISABLE_ASLR
  "Run the TSAN tests under setarch -R (ASLR workaround)" ON)

# ERNIC_TEST_LAUNCHER prefixes every add_test() COMMAND in tests/.
# Empty unless TSan needs the ASLR workaround below.
set(ERNIC_TEST_LAUNCHER "")

# TSan maps a large fixed shadow region at startup and aborts with
# "FATAL: ThreadSanitizer: unexpected memory mapping" when the loader
# has already randomised something into it.  Ubuntu ships 32 bits of
# mmap entropy, which is enough for that to happen on nearly every
# run, so the failure looks like a broken test suite rather than a
# configuration problem.
#
# setarch -R clears ADDR_NO_RANDOMIZE for the test and everything it
# spawns -- the script-driven tests start rocm-ernic as a child, and
# the personality flag is inherited across fork/exec.  It needs no
# privileges, unlike the equivalent `sysctl vm.mmap_rnd_bits=28`.
# build-and-test.yml wraps its tsan leg the same way.
if(ERNIC_USE_THREAD_SANITIZER AND ERNIC_TEST_DISABLE_ASLR)
    find_program(ERNIC_SETARCH_EXECUTABLE setarch)
    if(ERNIC_SETARCH_EXECUTABLE)
        set(ERNIC_TEST_LAUNCHER
          ${ERNIC_SETARCH_EXECUTABLE} ${CMAKE_SYSTEM_PROCESSOR} -R)
    else()
        message(WARNING
          "setarch not found; TSAN tests will run with ASLR enabled and "
          "will probably fail with \"unexpected memory mapping\".  Either "
          "install util-linux, or run `sudo sysctl vm.mmap_rnd_bits=28` "
          "and configure with -DERNIC_TEST_DISABLE_ASLR=OFF.")
    endif()
endif()

# Apply enabled sanitizer flags to ``target``.
function(ernic_add_sanitizers target)
    if(ERNIC_USE_SANITIZERS AND ERNIC_USE_THREAD_SANITIZER)
        message(FATAL_ERROR
          "ERNIC_USE_SANITIZERS is not compatible with "
          "ERNIC_USE_THREAD_SANITIZER")
    endif()

    if(ERNIC_USE_SANITIZERS)
        target_compile_options(${target} PRIVATE
          -fsanitize=address)
        target_link_options(${target} PRIVATE
          -fsanitize=address)
        target_compile_options(${target} PRIVATE
          -fsanitize=leak)
        target_link_options(${target} PRIVATE
          -fsanitize=leak)
        target_compile_options(${target} PRIVATE
          -fsanitize=undefined)
        target_link_options(${target} PRIVATE
          -fsanitize=undefined)

        target_compile_options(${target} PRIVATE
          -fno-omit-frame-pointer)
    endif()

    if(ERNIC_USE_THREAD_SANITIZER)
        target_compile_options(${target} PRIVATE
          -fsanitize=thread)
        target_link_options(${target} PRIVATE
          -fsanitize=thread)
    endif()
endfunction()
