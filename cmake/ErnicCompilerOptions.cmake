# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Set compiler flags on target based on the compiler in use.
#
# Follows the pattern from hipFile AISCompilerOptions.cmake and
# rocm-xio XIOCompilerOptions.cmake, adapted for C-only builds.

include(ErnicClangCompilerOptions)
include(ErnicGNUCompilerOptions)
include(ErnicSanitizers)

# Apply compiler warning and hardening flags to ``target``
# based on the compiler (GCC or Clang) used for each source.
function(ernic_set_compiler_flags target)
    get_target_property(sources ${target} SOURCES)
    foreach(source IN LISTS sources)
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

    ernic_add_sanitizers(${target})
endfunction()
