# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Compile checks for the Nix fuzz harnesses (nix/analysis/fuzz/).
#
# The harnesses are built for real by nix/analysis/fuzz.nix, which runs only
# behind the `run-fuzz` label or a manual dispatch. Nothing on an ordinary
# PR compiled them, so a changed parser signature under src/net/ could
# break them silently and stay broken until the next opt-in fuzz run.
#
# Here each harness is linked against tests/nix/fuzz_replay_main.c -- a
# plain main() rather than libFuzzer, so this builds under gcc as well as
# clang -- and registered with CTest. The build is what catches the drift;
# the replay is a cheap smoke test on top. Actual fuzzing stays in Nix.
#
# Keep the target list here in step with `harnesses` in
# nix/analysis/fuzz.nix: a harness added there but not here is once again
# only compiled by the opt-in workflow.

set(ERNIC_FUZZ_DIR ${PROJECT_SOURCE_DIR}/nix/analysis/fuzz)

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

    # The harness sources, and the ported .c files passed in ARGN, are ours
    # and are warning-clean, so they are built under the project flags --
    # keeping them there is the point of building them here. The harnesses
    # include the src/net headers by bare name.
    target_include_directories(${target} PRIVATE
        ${ERNIC_FUZZ_DIR}
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/net
        ${PROJECT_SOURCE_DIR}/src/qemu-compat/include
    )
    # Vendored header trees are SYSTEM includes so their warnings stay quiet.
    target_include_directories(${target} SYSTEM PRIVATE
        ${PROJECT_SOURCE_DIR}/third-party/qemu/hw/rdma
        ${GLIB2_INCLUDE_DIRS}
    )
    target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
    # Built with the sanitizers like the unit tests in the parent
    ernic_add_unit_test_sanitizers(${target})
    target_link_directories(${target} PRIVATE ${GLIB2_LIBRARY_DIRS})
    target_link_libraries(${target} PRIVATE ${GLIB2_LIBRARIES})

    # Project warning / hardening flags, matching the parent's
    # ERNIC_TEST_TARGETS loop.
    ernic_set_compiler_flags(${target})

    add_test(
        NAME fuzz-replay-${harness}
        COMMAND $<TARGET_FILE:${target}>
    )
    set_tests_properties(fuzz-replay-${harness} PROPERTIES TIMEOUT 30)
endfunction()

ernic_add_fuzz_replay(rdma_cm_proto
    ${PROJECT_SOURCE_DIR}/src/net/rdma_cm_proto.c
    ${PROJECT_SOURCE_DIR}/src/qemu-compat/error-report.c
)

# net_headers.h is header-only, so this harness links no extra sources.
ernic_add_fuzz_replay(net_headers)
