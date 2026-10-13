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
        ${PROJECT_SOURCE_DIR}/src/qemu
    )
    # Headers from outside the project are SYSTEM includes so their warnings
    # stay quiet.
    target_include_directories(${target} SYSTEM PRIVATE
        ${GLIB2_INCLUDE_DIRS}
    )
    target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
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

# The stats harness links the admin queue and includes the shared fake device
# from tests/, like tests/test_ionic_stats.c does, and defines
# LLVMFuzzerInitialize() to check its seed table before replaying.
ernic_add_fuzz_replay(ionic_stats ${PROJECT_SOURCE_DIR}/src/ionic_adminq.c)
target_include_directories(fuzz_replay_ionic_stats PRIVATE
    ${PROJECT_SOURCE_DIR}/tests
)
target_include_directories(fuzz_replay_ionic_stats SYSTEM PRIVATE
    ${VFIO_USER_INCLUDE_DIRS}
)
target_compile_definitions(fuzz_replay_ionic_stats PRIVATE
    FUZZ_HARNESS_HAS_INITIALIZE
)

# The NIC profile loader is pure text handling. Like the parsers above it
# needs no device, and it checks a seed table in LLVMFuzzerInitialize()
# before replaying.
ernic_add_fuzz_replay(profile_parse
    ${PROJECT_SOURCE_DIR}/src/ionic_profile.c
    ${PROJECT_SOURCE_DIR}/src/ionic_profiles.c
)
target_compile_definitions(fuzz_replay_profile_parse PRIVATE
    FUZZ_HARNESS_HAS_INITIALIZE
)

# Check private seed files and rejection of existing filesystem entries.
add_test(
    NAME fuzz-seed-files-profile_parse
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_fuzz_seed_files.sh
        $<TARGET_FILE:fuzz_replay_profile_parse>
)
set_tests_properties(fuzz-seed-files-profile_parse PROPERTIES TIMEOUT 30)

# The per-frame MAC class and Rx steering hash: pure functions of the frame
# bytes, with their own seed table.
ernic_add_fuzz_replay(eth_classify
    ${PROJECT_SOURCE_DIR}/src/ionic_emu_stats.c
    ${PROJECT_SOURCE_DIR}/src/ionic_eth_steer.c
)
target_compile_definitions(fuzz_replay_eth_classify PRIVATE
    FUZZ_HARNESS_HAS_INITIALIZE
)

# Check private seed files and rejection of existing filesystem entries.
add_test(
    NAME fuzz-seed-files-eth_classify
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_fuzz_seed_files.sh
        $<TARGET_FILE:fuzz_replay_eth_classify>
)
set_tests_properties(fuzz-seed-files-eth_classify PROPERTIES TIMEOUT 30)

# The fault spec parser is pure string handling; like the stats harness it
# checks a seed table in LLVMFuzzerInitialize() before replaying.
ernic_add_fuzz_replay(fault_parse
    ${PROJECT_SOURCE_DIR}/src/ionic_fault.c
)
target_compile_definitions(fuzz_replay_fault_parse PRIVATE
    FUZZ_HARNESS_HAS_INITIALIZE
)
