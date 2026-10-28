# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Warning flags for GNU gcc (C language)
#
# Adapted from hipObject HIPOBJGNUCompilerOptions.cmake for a C-only
# project. hipObject's C++-only flags (-Weffc++, -Wold-style-cast,
# -Wsuggest-override, -D_GLIBCXX_ASSERTIONS, etc.) are left out.
#
# https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html

include_guard(GLOBAL)

include(CheckCCompilerFlag)
include(ErnicFortifySource)

# Populate ``outvar`` with GCC warning flags appropriate for
# the given ``compiler_version``.
function(get_ernic_gnu_warning_flags outvar compiler_version)

    set(flags
        -Wall
        -Wextra

        # Avoid non-standard C behavior
        -pedantic

        # Stack protection
        -fstack-clash-protection
        -fstack-protector-strong

        # Keep null pointer checks the optimizer could prove redundant, and
        # don't assume that pointers to different types never alias
        -fno-delete-null-pointer-checks
        -fno-strict-aliasing

        # C-specific warnings
        -Wmissing-prototypes
        -Wstrict-prototypes
        -Wbad-function-cast
        # A goto or switch that jumps past a variable's initializer into
        # its scope
        -Wjump-misses-init
        -Wnested-externs
        -Wold-style-definition

        # General quality warnings
        -Walloca
        -Walloc-zero
        -Warray-bounds=2
        -Wattribute-alias=2
        # Plain -Wcast-align only warns on targets that require aligned
        # access, so it does nothing on x86-64. =strict warns everywhere.
        -Wcast-align=strict
        -Wcast-qual
        -Wconversion
        -Wdate-time
        -Wdouble-promotion
        -Wduplicated-branches
        -Wduplicated-cond
        -Wfloat-equal
        # Includes -Wformat-nonliteral, -Wformat-security, and -Wformat-y2k
        -Wformat=2
        -Wformat-overflow=2
        -Wformat-signedness
        -Wformat-truncation=2
        -Wimplicit-fallthrough=5
        -Winvalid-pch
        -Wlogical-op
        -Wmissing-declarations
        -Wmissing-include-dirs
        -Wnormalized
        -Wnull-dereference
        -Woverlength-strings

        # -Wpacked is deliberately left unset.  It fires when
        # __attribute__((packed)) does not change the layout, which is
        # exactly the case for a wire-format struct whose fields happen
        # to be naturally packed on this target.  Acting on it means
        # deleting the attribute, after which the next field added to
        # the struct silently reintroduces padding.  For a struct whose
        # layout is fixed by an external specification the redundancy is
        # the point.  (GCC-only for C; clang's -Wpacked covers only the
        # C++ -Wpacked-non-pod.)
        #
        # The related -Wpacked-not-aligned does catch a real defect -- a
        # member whose declared alignment the enclosing packed struct
        # drops -- but -Wall already enables it, so it is not repeated
        # here.
        #-Wpacked

        -Wpointer-arith
        -Wredundant-decls
        # Includes -Wshadow=local
        -Wshadow
        -Wshift-overflow=2
        # Warn about functions the stack protector doesn't cover
        -Wstack-protector
        -Wstrict-overflow=4
        -Wstringop-overflow=4
        -Wswitch-default
        -Wswitch-enum
        -Wtrampolines
        -Wundef
        -Wunknown-pragmas
        -Wunused-macros
        -Wvariadic-macros
        -Wvla
        # Give string literals the type const char[], so that writing
        # through one, or passing one as a char *, is diagnosed
        -Wwrite-strings
    )

    if(compiler_version VERSION_GREATER_EQUAL 10)
        set(flags
            -Warith-conversion
            ${flags}
        )
    endif()

    if(compiler_version VERSION_GREATER_EQUAL 11)
        set(flags
            # Zero the registers a function used when it returns, so their
            # contents don't outlive the call or serve as ROP gadgets
            -fzero-call-used-regs=used-gpr
            ${flags}
        )
    endif()

    if(compiler_version VERSION_GREATER_EQUAL 12)
        set(flags
            -Wbidi-chars=any
            # Warn about a variable -ftrivial-auto-var-init can't initialize,
            # such as one declared in a switch before its first case. Without
            # ERNIC_TRIVIAL_AUTO_VAR_INIT, it never warns.
            -Wtrivial-auto-var-init
            -Wuse-after-free=3
            ${flags}
        )

        if(ERNIC_TRIVIAL_AUTO_VAR_INIT)
            list(APPEND flags -ftrivial-auto-var-init=pattern)
        endif()

        # Only use _FORTIFY_SOURCE if the optimization level is -O2, -O3,
        # or -Os
        ernic_get_fortify_flags(fortify_flags)
        set(flags
            ${fortify_flags}
            ${flags}
        )
    endif()

    if(compiler_version VERSION_GREATER_EQUAL 13)
        set(flags
            # Strict flex arrays (helps ASAN, _FORTIFY_SOURCE, etc.)
            -fstrict-flex-arrays=3
            -Winvalid-utf8
            # Needs -fstrict-flex-arrays
            -Wstrict-flex-arrays
            ${flags}
        )
    endif()

    if(compiler_version VERSION_GREATER_EQUAL 14)
        set(flags
            -Wflex-array-member-not-at-end
            # A global variable defined with no earlier declaration, which
            # should be static or declared in a header
            -Wmissing-variable-declarations

            # -Wuseless-cast (C-valid from GCC 14) is deliberately left
            # unset.  Whether a cast is useless depends on the target's
            # type sizes: (int64_t)ts.tv_sec, (uintptr_t)u64 and
            # (unsigned long)u64 for "%lu" are no-ops on LP64 and required
            # on ILP32, so acting on it breaks 32-bit builds.
            #-Wuseless-cast
            ${flags}
        )
    endif()

    if(compiler_version VERSION_GREATER_EQUAL 15)
        set(flags
            # Zero the padding bits in every initializer of an automatic
            # variable, so stale stack memory can't leak through padding
            # (Clang has no equivalent)
            -fzero-init-padding-bits=all
            -Wtrailing-whitespace
            # The kind names the only whitespace allowed, so =spaces flags
            # tab indentation
            -Wleading-whitespace=spaces
            # C++-only before GCC 15
            -Wzero-as-null-pointer-constant
            ${flags}
        )
    endif()

    if(compiler_version VERSION_GREATER_EQUAL 16)
        set(flags
            -Wkeyword-macro
            ${flags}
        )
    endif()

    # Control-flow protection (Intel CET) only exists on x86
    check_c_compiler_flag(-fcf-protection=full
      ERNIC_C_COMPILER_SUPPORTS_CF_PROTECTION)
    if(ERNIC_C_COMPILER_SUPPORTS_CF_PROTECTION)
        list(APPEND flags -fcf-protection=full)
    endif()

    set(${outvar} ${flags} PARENT_SCOPE)

endfunction()
