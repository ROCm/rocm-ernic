# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Compile checks for the Nix fuzz harnesses (nix/analysis/fuzz/).
#
# The harnesses are built for real by nix/analysis/fuzz.nix, which runs only
# behind the `run-fuzz` label or a manual dispatch. Nothing on an ordinary
# PR compiled them, so a changed parser signature under
# src/from-qemu/utils/ could break them silently and stay broken until the
# next opt-in fuzz run.
#
# Here each harness is linked against tests/nix/fuzz_replay_main.c -- a
# plain main() rather than libFuzzer, so this builds under gcc as well as
# clang -- and registered with CTest. The build is what catches the drift;
# the replay is a cheap smoke test on top. Actual fuzzing stays in Nix.
#
# Keep the target list here in step with `harnesses` in
# nix/analysis/fuzz.nix: a harness added there but not here is once again
# only compiled by the opt-in workflow.

set(ERNIC_FUZZ_DIR ${CMAKE_SOURCE_DIR}/nix/analysis/fuzz)

# Source-file properties are directory-scoped, so the parent's -w on
# ERNIC_TEST_PORTED_SOURCES does not reach here. Re-silence the ported
# sources these targets compile; without it the QEMU-ported C is held to
# the project warning flags and fails under ERNIC_WERROR.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/rdma_cm_proto.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/dhcp_server.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/error-report.c
    PROPERTIES COMPILE_OPTIONS "-w"
)

# Mirrors ERNIC_PARSER_TEST_SAN in the parent: ASan + UBSan unless the
# project is already built with TSan, which is incompatible.
set(ERNIC_FUZZ_REPLAY_SAN "")
if(NOT ERNIC_USE_THREAD_SANITIZER)
    set(ERNIC_FUZZ_REPLAY_SAN
        -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

# ernic_add_fuzz_replay(<harness> [extra ported sources...])
#
# Builds fuzz_replay_<harness> from nix/analysis/fuzz/fuzz_<harness>.c plus
# the driver and any ported .c files the harness links, then registers it
# with CTest.
function(ernic_add_fuzz_replay harness)
    set(target fuzz_replay_${harness})

    add_executable(${target}
        ${ERNIC_FUZZ_DIR}/fuzz_${harness}.c
        fuzz_replay_main.c
        ${ARGN}
    )

    # The harness sources are ours and are warning-clean, so they are NOT
    # silenced the way ERNIC_TEST_PORTED_SOURCES is -- keeping them under
    # the project flags is the point of building them here. The ported .c
    # files passed in ARGN are silenced per-source by the parent scope's
    # set_source_files_properties() on ERNIC_TEST_PORTED_SOURCES.
    target_include_directories(${target} PRIVATE
        ${ERNIC_FUZZ_DIR}
        ${CMAKE_SOURCE_DIR}/src
    )
    # Ported header trees are SYSTEM includes so their warnings stay quiet.
    target_include_directories(${target} SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/src/from-qemu
        ${CMAKE_SOURCE_DIR}/src/from-qemu/utils
        ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
        ${GLIB2_INCLUDE_DIRS}
    )
    target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
    target_compile_options(${target} PRIVATE ${ERNIC_FUZZ_REPLAY_SAN})
    target_link_options(${target} PRIVATE ${ERNIC_FUZZ_REPLAY_SAN})
    target_link_directories(${target} PRIVATE ${GLIB2_LIBRARY_DIRS})
    target_link_libraries(${target} PRIVATE ${GLIB2_LIBRARIES})

    # Project warning / hardening flags, matching the parent's
    # ERNIC_TEST_TARGETS loop.
    ernic_set_compiler_flags(${target})
    if(ERNIC_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()

    add_test(
        NAME fuzz-replay-${harness}
        COMMAND $<TARGET_FILE:${target}>
    )
    set_tests_properties(fuzz-replay-${harness} PROPERTIES TIMEOUT 30)
endfunction()

ernic_add_fuzz_replay(rdma_cm_proto
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/rdma_cm_proto.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/error-report.c
)

ernic_add_fuzz_replay(dhcp_server
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/dhcp_server.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/error-report.c
)
# Only this harness defines LLVMFuzzerInitialize(); the driver calls it
# behind this define rather than a weak symbol, which gcc and clang treat
# differently when statically linked.
target_compile_definitions(fuzz_replay_dhcp_server PRIVATE
    FUZZ_HARNESS_HAS_INITIALIZE
)

# net_headers.h is header-only, so this harness links no extra sources.
ernic_add_fuzz_replay(net_headers)
