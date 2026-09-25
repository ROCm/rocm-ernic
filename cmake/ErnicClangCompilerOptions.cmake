# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

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

        # ...with one exception.  The vendored rdma_utils.h uses the GNU
        # ``, ##__VA_ARGS__`` comma-swallowing extension in the
        # rdma_*_report() macros.  The header is a SYSTEM include, but
        # clang still reports the paste wherever the macros are expanded
        # outside a system header -- that is, throughout our own code,
        # where -w on the vendored .c files does not reach.  Rather than
        # patch vendored code, drop just this one sub-warning of -Wgnu.
        -Wno-gnu-zero-variadic-macro-arguments

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
        -Wassign-enum
        -Watomic-implicit-seq-cst
        -Wbinary-literal
        -Wcast-align
        -Wcast-function-type
        -Wcast-qual
        -Wcomma
        -Wconditional-uninitialized
        -Wconversion
        -Wcstring-format-directive
        -Wdate-time
        -Wdisabled-macro-expansion
        -Wdocumentation
        -Wdouble-promotion
        -Wduplicate-enum
        -Wendif-labels
        -Wexpansion-to-defined
        -Wextra-semi
        -Wextra-semi-stmt
        -Wfloat-equal
        -Wformat=2
        -Wformat-non-iso
        -Wformat-pedantic
        -Wformat-security
        -Wformat-type-confusion
        -Wfour-char-constants
        -Wfuse-ld-path
        -Wglobal-constructors
        -Wimplicit-fallthrough
        -Wincompatible-function-pointer-types-strict
        -Wincomplete-module
        -Winvalid-or-nonexistent-directory
        -Wloop-analysis
        -Wmain
        -Wmain-return-type
        -Wmax-tokens
        -Wmicrosoft
        -Wmissing-include-dirs
        -Wmissing-noreturn
        -Wmissing-variable-declarations
        -Wnewline-eof
        -Wnon-gcc
        -Wnonportable-system-include-path
        -Wnull-dereference
        -Wnullable-to-nonnull-conversion
        -Wopenmp
        -Wover-aligned

        # -Wpacked is deliberately left unset.  On GCC it fires when
        # __attribute__((packed)) does not change the layout, which is
        # exactly the case for a wire-format struct whose fields happen
        # to be naturally packed on this target; acting on it means
        # deleting the attribute, after which the next field added to
        # the struct silently reintroduces padding.  clang's -Wpacked is
        # a near no-op here in any case -- its only member,
        # -Wpacked-non-pod, is C++-only.
        #-Wpacked

        -Wpartial-availability
        -Wpointer-arith
        -Wpoison-system-directories
        -Wpragmas
        -Wquoted-include-in-framework-header
        -Wredundant-parens
        -Wreserved-identifier
        -Wsequence-point
        -Wshadow-all
        -Wshift-sign-overflow
        -Wsigned-enum-bitfield
        -Wsource-uses-openmp
        -Wstatic-in-inline
        -Wswitch-default
        -Wswitch-enum
        -Wtautological-constant-in-range-compare
        -Wtype-limits
        -Wunaligned-access
        -Wundef
        -Wundef-prefix
        -Wunguarded-availability
        -Wunreachable-code-aggressive
        -Wused-but-marked-unused
        -Wvariadic-macros
        -Wvector-conversion
        -Wvla

        # QEMU-ported code has many unused parameters
        -Wno-unused-parameter
    )

    if(compiler_version VERSION_GREATER_EQUAL 18.1.6)
        set(flags
            -Wnonportable-private-system-apinotes-path
            -Wopenacc
            -Wsource-uses-openacc
            ${flags}
        )
    endif()

    if(compiler_version VERSION_GREATER_EQUAL 19.1)
        set(flags
            -Wformat-signedness
            ${flags}
        )
    endif()

    if(compiler_version VERSION_GREATER_EQUAL 20.1)
        set(flags
            -Wdecls-in-multiple-modules
            -Wvariadic-macro-arguments-omitted
            ${flags}
        )
    endif()

    if(compiler_version VERSION_GREATER_EQUAL 21.0)
        set(flags
            -Wthread-safety-pointer
            -Wshift-bool
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
