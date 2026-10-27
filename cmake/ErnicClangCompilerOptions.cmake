# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Warning flags for llvm/clang (C language)
#
# Adapted from hipObject HIPOBJClangCompilerOptions.cmake for a C-only
# project. hipObject's C++ and Objective-C flags (-Weffc++,
# -Wexit-time-destructors, the selector and method warnings, etc.) are
# left out, as are the CUDA compatibility warnings.
#
# https://clang.llvm.org/docs/DiagnosticsReference.html

include_guard(GLOBAL)

include(CheckCCompilerFlag)
include(CheckCSourceCompiles)
include(CMakePushCheckState)
include(ErnicFortifySource)

# Check whether calls to the C library's fortified functions trip
# -Wused-but-marked-unused
#
# With _FORTIFY_SOURCE, glibc 2.40 and later give clang printf-family
# wrappers that are declared static __attribute__((__unused__)), so
# every call to fprintf, snprintf, and the like warns. The result is
# cached, so the check only runs once.
function(ernic_check_fortify_used_but_marked_unused outvar)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS
      "-O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -Werror=used-but-marked-unused")
    check_c_source_compiles("
        #include <stdio.h>
        int main(void)
        {
            char buf[8];
            snprintf(buf, sizeof(buf), \"%d\", 1);
            fprintf(stderr, \"%s\", buf);
            return 0;
        }" ERNIC_FORTIFY_ALLOWS_USED_BUT_MARKED_UNUSED)
    cmake_pop_check_state()
    set(${outvar} ${ERNIC_FORTIFY_ALLOWS_USED_BUT_MARKED_UNUSED} PARENT_SCOPE)
endfunction()

# Check whether the C library's own macros trip -Wdisabled-macro-expansion
#
# Some glibc macros expand to a call of the function they're named
# after, and clang reports every use of them outside a system header:
#
#   - glibc 2.43 and later wrap strchr, strstr, memchr, and similar
#     functions in const-preserving macros of the same name
#   - With _FORTIFY_SOURCE, glibc before 2.40 makes printf, fprintf,
#     snprintf, and the like macros when compiling C with clang, so the
#     self-referencing stderr macro is expanded inside their arguments
#
# ``flags`` are added to the compile, and ``resultvar`` names the cache
# variable that holds the result, so each set of flags is only checked
# once.
function(ernic_check_libc_disabled_macro_expansion outvar flags resultvar)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS "${flags} -Werror=disabled-macro-expansion")
    # The feature-test macros ernic_set_compiler_flags() gives every target
    set(CMAKE_REQUIRED_DEFINITIONS -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE)
    check_c_source_compiles("
        #include <stdio.h>
        #include <string.h>
        int main(void)
        {
            char buf[8];
            snprintf(buf, sizeof(buf), \"%d\", 1);
            fprintf(stderr, \"%s\", buf);
            return strchr(buf, '1') != NULL && strstr(buf, \"1\") != NULL
                && memchr(buf, '1', sizeof(buf)) != NULL;
        }" ${resultvar})
    cmake_pop_check_state()
    set(${outvar} ${${resultvar}} PARENT_SCOPE)
endfunction()

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

        # Zero the registers a function used when it returns, so their
        # contents don't outlive the call or serve as ROP gadgets
        -fzero-call-used-regs=used-gpr

        # Keep null pointer checks the optimizer could prove redundant, and
        # don't assume that pointers to different types never alias
        -fno-delete-null-pointer-checks
        -fno-strict-aliasing

        # C-specific warnings
        -Wimplicit-function-declaration
        -Wmissing-prototypes
        -Wstrict-prototypes
        -Wbad-function-cast
        -Wnested-externs
        -Wold-style-definition

        # General quality warnings
        -Walloca
        -Warray-bounds-pointer-arithmetic
        -Wassign-enum
        -Watomic-implicit-seq-cst
        -Wbinary-literal
        -Wcast-align
        -Wcast-function-type
        -Wcast-qual
        -Wcomma
        -Wcompound-token-split-by-space
        -Wconditional-uninitialized
        -Wconversion
        -Wcstring-format-directive
        -Wdate-time
        -Wdisabled-macro-expansion
        -Wdocumentation
        -Wdocumentation-pedantic
        -Wdouble-promotion
        -Wduplicate-enum
        -Wendif-labels
        -Wexpansion-to-defined
        -Wextra-semi
        -Wextra-semi-stmt
        -Wfloat-equal
        # Includes -Wformat-nonliteral, -Wformat-security, and -Wformat-y2k
        -Wformat=2
        -Wformat-non-iso
        -Wformat-pedantic
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
        #-Wunsafe-buffer-usage # TODO: Fix issues
        -Wunused-macros
        -Wused-but-marked-unused
        -Wvariadic-macros
        -Wvector-conversion
        -Wvla
        # Give string literals the type const char[], so that writing
        # through one, or passing one as a char *, is diagnosed
        -Wwrite-strings
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

    if(compiler_version VERSION_GREATER_EQUAL 22.1)
        set(flags
            -Wshadow-header
            ${flags}
        )
    endif()

    if(compiler_version VERSION_GREATER_EQUAL 23.1)
        set(flags
            # Don't assume a bool loaded from memory holds 0 or 1, so a
            # corrupted bool can't steer the optimized code
            -fno-strict-bool

            -Wnonportable-include-path-separator
            ${flags}
        )
    endif()

    # Only use _FORTIFY_SOURCE if the optimization level is -O2, -O3, or -Os
    ernic_get_fortify_flags(fortify_flags)

    # Drop -Wused-but-marked-unused when the fortified C library functions
    # trip it, since there's nothing to fix in our code. With a
    # multi-config generator, this drops it from every configuration.
    if(fortify_flags)
        ernic_check_fortify_used_but_marked_unused(fortify_allows_warning)
        if(NOT fortify_allows_warning)
            list(REMOVE_ITEM flags -Wused-but-marked-unused)
        endif()
    endif()

    # Drop -Wdisabled-macro-expansion when the C library's own macros trip
    # it, with or without _FORTIFY_SOURCE, since there's nothing to fix in
    # our code. With a multi-config generator, this drops it from every
    # configuration.
    ernic_check_libc_disabled_macro_expansion(libc_allows_warning ""
      ERNIC_LIBC_ALLOWS_DISABLED_MACRO_EXPANSION)
    if(libc_allows_warning AND fortify_flags)
        ernic_check_libc_disabled_macro_expansion(libc_allows_warning
          "-O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3"
          ERNIC_FORTIFY_ALLOWS_DISABLED_MACRO_EXPANSION)
    endif()
    if(NOT libc_allows_warning)
        list(REMOVE_ITEM flags -Wdisabled-macro-expansion)
    endif()

    set(flags
        ${fortify_flags}
        ${flags}
    )

    if(ERNIC_TRIVIAL_AUTO_VAR_INIT)
        list(APPEND flags -ftrivial-auto-var-init=pattern)
    endif()

    # Control-flow protection (Intel CET) only exists on x86
    check_c_compiler_flag(-fcf-protection=full
      ERNIC_C_COMPILER_SUPPORTS_CF_PROTECTION)
    if(ERNIC_C_COMPILER_SUPPORTS_CF_PROTECTION)
        list(APPEND flags -fcf-protection=full)
    endif()

    set(${outvar} ${flags} PARENT_SCOPE)

endfunction()
