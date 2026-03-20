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

if(ERNIC_USE_THREAD_SANITIZER)
    message(WARNING
      "TSAN has known problems with higher levels of entropy, "
      "try using `sudo sysctl vm.mmap_rnd_bits=28` if you "
      "encounter errors concerning unexpected memory mappings.")
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
