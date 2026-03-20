# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Warning flags for GNU gcc (C language)
#
# Adapted from hipFile AISGNUCompilerOptions.cmake and
# rocm-xio XIOGNUCompilerOptions.cmake for a C-only project.
#
# https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html

# Populate ``outvar`` with GCC warning flags appropriate for
# the given ``compiler_version``.
function(get_ernic_gnu_warning_flags outvar compiler_version)

    set(flags
        -Wall
        -Wextra

        # Stack protection
        -fstack-clash-protection
        -fstack-protector-strong

        # C-specific warnings
        -Wimplicit-function-declaration
        -Wmissing-prototypes
        -Wstrict-prototypes
        -Wbad-function-cast
        -Wnested-externs
        -Wold-style-definition
        -Wold-style-declaration

        # General quality warnings
        -Walloca
        -Walloc-zero
        -Warray-bounds=2
        -Wcast-align
        -Wcast-qual
        -Wconversion
        -Wdate-time
        -Wdouble-promotion
        -Wduplicated-branches
        -Wduplicated-cond
        -Wfloat-equal
        -Wformat=2
        -Wformat-nonliteral
        -Wformat-overflow=2
        -Wformat-security
        -Wformat-signedness
        -Wformat-truncation=2
        -Wformat-y2k
        -Wlogical-op
        -Wmissing-declarations
        -Wnormalized
        -Wnull-dereference
        -Wpacked
        -Wpointer-arith
        -Wredundant-decls
        -Wshadow
        -Wshadow-local
        -Wshift-overflow=2
        -Wstrict-overflow=4
        -Wswitch-default
        -Wswitch-enum
        -Wtrampolines
        -Wundef
        -Wuninitialized
        -Wunknown-pragmas
        -Wunsafe-loop-optimizations
        -Wunused
        -Wunused-macros
        -Wvla

        # QEMU-ported code has many unused parameters
        -Wno-unused-parameter
    )

    if(compiler_version VERSION_GREATER_EQUAL 12)
        set(flags
            -Wbidi-chars=any
            -Wtrivial-auto-var-init
            ${flags}
        )

        string(JOIN " " MYCFLAGS ${CMAKE_C_FLAGS}
          ${CMAKE_C_FLAGS_${CMAKE_BUILD_TYPE}})
        if(MYCFLAGS MATCHES "-O[2-3s]")
            set(flags
                -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3
                ${flags}
            )
        endif()
    endif()

    if(compiler_version VERSION_GREATER_EQUAL 13)
        set(flags
            -fstrict-flex-arrays=3
            -Winvalid-utf8
            ${flags}
        )
    endif()

    if(compiler_version VERSION_GREATER_EQUAL 14)
        set(flags
            -Walloc-size
            -Wcalloc-transposed-args
            -Wflex-array-member-not-at-end
            ${flags}
        )
    endif()

    if(compiler_version VERSION_GREATER_EQUAL 15)
        set(flags
            -Wtrailing-whitespace
            -Wleading-whitespace=tabs
            ${flags}
        )
    endif()

    set(${outvar} ${flags} PARENT_SCOPE)

endfunction()
