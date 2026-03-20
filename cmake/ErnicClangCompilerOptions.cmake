# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Warning flags for llvm/clang (C language)
#
# Adapted from hipFile AISClangCompilerOptions.cmake and
# rocm-xio XIOClangCompilerOptions.cmake for a C-only project.
#
# https://clang.llvm.org/docs/DiagnosticsReference.html

# Populate ``outvar`` with clang warning flags appropriate for
# the given ``compiler_version``.
function(get_ernic_clang_warning_flags outvar compiler_version)

    set(flags
        -Wall
        -Wextra

        -pedantic

        # Don't bake gcc-isms into the code
        -Wgnu

        -Wdeprecated

        # Thread-safety analysis
        -Wthread-safety
        -Wthread-safety-beta
        -Wthread-safety-negative
        -Wthread-safety-verbose

        # Stack protection
        -fstack-clash-protection
        -fstack-protector-strong

        -fstrict-flex-arrays=3
        -ftrivial-auto-var-init=pattern

        # C-specific warnings
        -Wimplicit-function-declaration
        -Wmissing-prototypes
        -Wstrict-prototypes
        -Wbad-function-cast
        -Wnested-externs
        -Wold-style-definition

        # General quality warnings
        -Warray-bounds-pointer-arithmetic
        -Wcast-align
        -Wcast-qual
        -Wconditional-uninitialized
        -Wconversion
        -Wdate-time
        -Wdouble-promotion
        -Wduplicate-enum
        -Wfloat-equal
        -Wformat=2
        -Wformat-security
        -Wimplicit-fallthrough
        -Wmissing-include-dirs
        -Wmissing-variable-declarations
        -Wnull-dereference
        -Wpacked
        -Wpointer-arith
        -Wredundant-parens
        -Wshadow-all
        -Wshift-sign-overflow
        -Wswitch-default
        -Wtype-limits
        -Wundef
        -Wunreachable-code-aggressive
        -Wvla

        # QEMU-ported code has many unused parameters
        -Wno-unused-parameter
    )

    if(compiler_version VERSION_GREATER_EQUAL 19.1)
        set(flags
            -Wformat-signedness
            ${flags}
        )
    endif()

    # _FORTIFY_SOURCE needs optimisation
    string(JOIN " " MYCFLAGS ${CMAKE_C_FLAGS}
      ${CMAKE_C_FLAGS_${CMAKE_BUILD_TYPE}})
    if(MYCFLAGS MATCHES "-O[2-3s]")
        set(flags
            -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3
            ${flags}
        )
    endif()

    set(${outvar} ${flags} PARENT_SCOPE)

endfunction()
