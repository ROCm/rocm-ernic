# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Tests build configuration

# ernic_add_test_program() and ernic_add_unit_test()
include(ErnicTesting)

# No test here sets SKIP_RETURN_CODE. Every registered test needs no VM,
# no RDMA device and no root, so a test that can't run (a missing server,
# a missing tool) is broken, and has to fail rather than show up as
# skipped. A test that needs a device isn't registered; see
# test_data_transfer below.

# -- Sources several tests link ----------------------------------
# Each family of unit tests below links the same part of src/. A test
# needing a different set lists its own.

# The ionic Ethernet emulator and what it calls: guest DMA, NIC profiles,
# the LIF counters, Rx steering and fault injection
set(ERNIC_TEST_IONIC_ETH_SOURCES
    ${PROJECT_SOURCE_DIR}/src/ernic_dma.c
    ${PROJECT_SOURCE_DIR}/src/ionic/ionic_eth_emu.c
    ${PROJECT_SOURCE_DIR}/src/ionic/ionic_profile.c
    ${PROJECT_SOURCE_DIR}/src/ionic/ionic_profiles.c
    ${PROJECT_SOURCE_DIR}/src/ionic/ionic_emu_stats.c
    ${PROJECT_SOURCE_DIR}/src/ionic/ionic_eth_steer.c
    ${PROJECT_SOURCE_DIR}/src/ionic/ionic_fault.c
)

# The ionic admin queue, with guest DMA and fault injection
set(ERNIC_TEST_ADMINQ_SOURCES
    ${PROJECT_SOURCE_DIR}/src/ernic_dma.c
    ${PROJECT_SOURCE_DIR}/src/ionic/ionic_adminq.c
    ${PROJECT_SOURCE_DIR}/src/ionic/ionic_fault.c
)

# The ionic RDMA data path and its NVMe-oF and S3 paths, with guest DMA
# and the log
set(ERNIC_TEST_DATAPATH_SOURCES
    ${PROJECT_SOURCE_DIR}/src/ernic_dma.c
    ${PROJECT_SOURCE_DIR}/src/ernic_log.c
    ${PROJECT_SOURCE_DIR}/src/ionic/ionic_datapath.c
    ${PROJECT_SOURCE_DIR}/src/ionic/ionic_datapath_nvmeof.c
    ${PROJECT_SOURCE_DIR}/src/ionic/ionic_datapath_s3.c
)

# The P4 engine's loader and pipeline, without the device or the externs
set(ERNIC_TEST_P4_ENGINE_SOURCES
    ${PROJECT_SOURCE_DIR}/src/p4/p4_engine.c
    ${PROJECT_SOURCE_DIR}/src/p4/p4_exec.c
    ${PROJECT_SOURCE_DIR}/src/p4/p4_json.c
    ${PROJECT_SOURCE_DIR}/src/p4/p4_table.c
)
# Real p4c artifacts, so the tests that load a program hold the loader to
# what the compiler actually emits rather than to what the format
# documentation says.
set(ERNIC_TEST_P4_FIXTURE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data/p4)

# The TCP backend's sources. The list comes from the top-level
# CMakeLists.txt, so the tests keep linking what the server links, plus
# the GNU extensions it calls.
set(ERNIC_TCP_TEST_SOURCES ${TCP_BACKEND_SOURCES})
list(TRANSFORM ERNIC_TCP_TEST_SOURCES PREPEND "${PROJECT_SOURCE_DIR}/")
list(APPEND ERNIC_TCP_TEST_SOURCES ${PROJECT_SOURCE_DIR}/src/ernic_gnu.c)

# -- test_pci_client (no extra dependencies) ---------------------
ernic_add_test_program(test_pci_client SOURCES test_pci_client.c)

# -- test_data_transfer (requires libibverbs) --------------------
ernic_add_test_program(test_data_transfer
    SOURCES test_data_transfer.c
    DEPS ibverbs
)

# -- ernic-rdma-errors (guest tool; requires libibverbs) ---------
# Run in a guest on an ionic_rdma device, not by ctest: it makes the RDMA
# errors the emulator counts happen and checks hw_counters moved.
ernic_add_test_program(ernic-rdma-errors
    SOURCES guest/ernic_rdma_errors.c
    DEPS ibverbs
)

# -- test_rdma_cm (requires libibverbs) --------------------------
ernic_add_test_program(test_rdma_cm
    SOURCES test_rdma_cm.c
    DEPS ibverbs
)

# -- Guest-only SRQ and UD tests (require libibverbs) ------------
# Built here, never registered with CTest, for the reason the comment
# further down gives for test_data_transfer and test_rdma_cm: they need a
# real RDMA device, and on a host without one they exit 77, which CTest
# scores as a pass. They run for real in the guest, from system-tests.yml.
#
# Building them is still worth it. Until they were added here they were in
# no target at all, so a syntax or type error in any of them surfaced only
# when gcc ran inside the VM, well into a 60-minute VM job. Now
# the hosted build legs catch it in seconds, under the same warning and
# hardening flags as every other test target.
set(ERNIC_GUEST_ONLY_TESTS
    test_srq
    test_srq_transfer
    test_ud_transfer
    test_ud_sq_reclaim
    test_ud_mesh
)
foreach(guest_test IN LISTS ERNIC_GUEST_ONLY_TESTS)
    ernic_add_test_program(${guest_test}
        SOURCES ${guest_test}.c
        DEPS ibverbs
    )
endforeach()
# test_ud_mesh accepts its out-of-band connection with accept4(). The
# guest build in ansible/playbooks/sanity-tests.yml links the same file.
target_sources(test_ud_mesh PRIVATE ${PROJECT_SOURCE_DIR}/src/ernic_gnu.c)

# -- Unit tests --------------------------------------------------

# -- test_write_imm (requires libibverbs) ------------------------
ernic_add_test_program(test_write_imm
    SOURCES test_write_imm.c
    DEPS ibverbs
)

# Fixture-backed tests for the C-side emulated RDMA device lookup.
ernic_add_test_program(test_ernic_device
    SOURCES test_ernic_device.c
    HEADER_DEPS ibverbs
)
set(ERNIC_DEVICE_FIXTURE_ROOT
    ${CMAKE_CURRENT_BINARY_DIR}/ernic-device-fixture)
target_compile_definitions(test_ernic_device PRIVATE
    ERNIC_SYSFS_INFINIBAND="${ERNIC_DEVICE_FIXTURE_ROOT}/class/infiniband"
    ERNIC_TEST_FIXTURE_ROOT="${ERNIC_DEVICE_FIXTURE_ROOT}"
)

# The bounds check every guest DMA address and length passes before it
# reaches vfu_addr_to_sgl(). Header-only, so it links nothing.
ernic_add_test_program(test_dma_range SOURCES test_dma_range.c)

# Which part of BAR0 an access goes to, including the out-of-bounds
# accesses libvfio-user refuses before the server sees them. Header-only.
ernic_add_test_program(ernic-test-ionic-bar SOURCES test_ionic_bar.c)

# The one path between host buffers and guest memory (ernic_dma.c), against
# fake_vfu.c's guest memory: transfers across region boundaries, ones that
# need more scatter/gather entries than one list holds, and every failure
# libvfio-user can return.
ernic_add_test_program(ernic-test-dma
    SOURCES
        test_ernic_dma.c
        fake_vfu.c
        ${PROJECT_SOURCE_DIR}/src/ernic_dma.c
    HEADER_DEPS vfio-user
)

# The TAP backend (ionic_eth_net.c) refuses an interface name too long for
# the kernel rather than cutting it short. Needs no TAP device or privileges.
ernic_add_test_program(ernic-test-ionic-eth-net
    SOURCES
        test_ionic_eth_net.c
        ${PROJECT_SOURCE_DIR}/src/ionic/ionic_eth_net.c
)

# -- Parser unit tests -------------------------------------------
# Strict integer parser (parse_int.h) unit test (header-only).
ernic_add_test_program(test_parse_int SOURCES test_parse_int.c)

# Byte-order accessor (byteorder.h) unit test (header-only).
ernic_add_test_program(ernic-test-byteorder SOURCES test_byteorder.c)

# SipHash-2-4 (ernic_siphash.h) against the reference test vectors.
ernic_add_test_program(ernic-test-siphash
    SOURCES
        test_siphash.c
        ${PROJECT_SOURCE_DIR}/src/ernic_siphash.c
)

# P4 engine test. The engine is frames-in/frames-out, so this links the
# engine's own translation units and nothing else -- no device, no guest
# memory.
if(ERNIC_BUILD_P4)
    ernic_add_test_program(ernic-test-p4-engine
        SOURCES
            test_p4_engine.c
            ${ERNIC_TEST_P4_ENGINE_SOURCES}
        DEPS json-c
    )
    target_compile_definitions(ernic-test-p4-engine PRIVATE
        P4_FIXTURE_DIR="${ERNIC_TEST_P4_FIXTURE_DIR}")

    # Loading a program when memory runs out. It fails the loader's
    # allocations on demand with the S3 tests' wrappers (s3_alloc_fail.h),
    # and json_tokener_new_ex() with its own, since json-c allocates the
    # tokener where --wrap can't reach.
    ernic_add_test_program(ernic-test-p4-alloc-fail
        SOURCES
            test_p4_alloc_fail.c
            s3_alloc_fail.c
            ${ERNIC_TEST_P4_ENGINE_SOURCES}
        DEPS json-c
    )
    target_compile_definitions(ernic-test-p4-alloc-fail PRIVATE
        P4_FIXTURE_DIR="${ERNIC_TEST_P4_FIXTURE_DIR}")
    target_link_options(ernic-test-p4-alloc-fail PRIVATE
        "LINKER:--wrap=malloc,--wrap=calloc,--wrap=realloc"
        "LINKER:--wrap=strdup,--wrap=getentropy"
        "LINKER:--wrap=json_tokener_new_ex")

    # OCP FP8 E4M3 arithmetic: the extern the pipeline calls, since P4
    # itself has no floating-point type. Pure computation, no device.
    ernic_add_test_program(ernic-test-p4-fp8
        SOURCES
            test_p4_fp8.c
            ${PROJECT_SOURCE_DIR}/src/p4/p4_fp8.c
    )

    # The formats the conversion extern moves between, in either
    # direction. Links p4_fp8.c as well, because the strongest check it
    # makes is that the generic codec reproduces that one exactly for
    # E4M3 -- which is how it inherits p4_fp8.c's own validation against
    # ml_dtypes.
    ernic_add_test_program(ernic-test-p4-fmt
        SOURCES
            test_p4_fmt.c
            ${PROJECT_SOURCE_DIR}/src/p4/p4_fmt.c
            ${PROJECT_SOURCE_DIR}/src/p4/p4_fp8.c
    )
endif()

# NVMe-oF controller test. The controller core takes its DMA through a
# callback pair, so the test links the real translation unit and stands a
# byte array in for guest memory -- no VM, no RDMA device, no server.
ernic_add_test_program(test_nvmeof_target
    SOURCES
        test_nvmeof_target.c
        ${PROJECT_SOURCE_DIR}/src/nvmeof/nvmeof_target.c
        ${PROJECT_SOURCE_DIR}/src/ernic_log.c
    HEADER_DEPS vfio-user
)

# IB CM responder test. Feeds it the CM MADs a guest running rdma_cm
# emits and dissects the replies; no MAD ever reaches a wire.
ernic_add_test_program(test_nvmeof_cm
    SOURCES
        test_nvmeof_cm.c
        ${PROJECT_SOURCE_DIR}/src/nvmeof/nvmeof_cm.c
        ${PROJECT_SOURCE_DIR}/src/nvmeof/nvmeof_target.c
        ${PROJECT_SOURCE_DIR}/src/ernic_log.c
    HEADER_DEPS vfio-user
)

# S3-over-RDMA token codec test. The encoding is a wire contract shared
# with hipObject, so this links nothing but the codec itself.
ernic_add_test_program(test_s3_token
    SOURCES
        test_s3_token.c
        ${PROJECT_SOURCE_DIR}/src/s3/s3_token.c
)

# S3 control-plane HTTP subset test: buffers in, buffers out, no socket.
ernic_add_test_program(test_s3_http
    SOURCES
        test_s3_http.c
        ${PROJECT_SOURCE_DIR}/src/s3/s3_http.c
)

# S3 object store test. Like the NVMe-oF one, the target takes its DMA
# through a callback pair, so a byte array stands in for guest memory and
# no VM, RDMA device or server process is involved.
ernic_add_test_program(test_s3_target
    SOURCES
        test_s3_target.c
        ${PROJECT_SOURCE_DIR}/src/s3/s3_target.c
        ${PROJECT_SOURCE_DIR}/src/s3/s3_http.c
        ${PROJECT_SOURCE_DIR}/src/s3/s3_token.c
)

# In-band TCP endpoint test. It drives the endpoint with hand-built
# Ethernet frames and dissects what comes back, so the ARP exchange, the
# handshake, segment reassembly and the checksums are all checked against
# an independent implementation in the test.
ernic_add_test_program(test_s3_tcp
    SOURCES
        test_s3_tcp.c
        ${PROJECT_SOURCE_DIR}/src/ernic_siphash.c
        ${PROJECT_SOURCE_DIR}/src/s3/s3_tcp.c
        ${PROJECT_SOURCE_DIR}/src/s3/s3_target.c
        ${PROJECT_SOURCE_DIR}/src/s3/s3_http.c
        ${PROJECT_SOURCE_DIR}/src/s3/s3_token.c
)

# The S3 tests fail allocations on demand (s3_alloc_fail.h). The linker
# routes the calls in their own objects through wrappers, which pass every
# call they don't fail to the real function; libc's own calls are untouched.
foreach(s3_test test_s3_http test_s3_target test_s3_tcp)
    target_sources(${s3_test} PRIVATE s3_alloc_fail.c)
    target_link_options(${s3_test} PRIVATE
        "LINKER:--wrap=malloc,--wrap=calloc,--wrap=realloc"
        "LINKER:--wrap=strdup,--wrap=getentropy")
endforeach()

# ionic MSI-X mask/pending test. It links the ionic emulator, reaches the
# per-vector interrupt state through ionic_eth_emu_priv.h, and stubs
# vfu_irq_trigger() with a per-vector counter, so no vfio-user link or live
# context is needed.
ernic_add_test_program(test_ionic_intr_pending
    SOURCES
        test_ionic_intr_pending.c
        ${ERNIC_TEST_IONIC_ETH_SOURCES}
    DEPS glib
    HEADER_DEPS vfio-user ibverbs
)

# ionic PORT_INIT port-info test. It links the eth emulator, calls its
# handlers through ionic_eth_emu_priv.h, and backs the DMA entry points with
# a plain buffer, so the structure the driver reads its link speed out of
# can be inspected directly -- no guest, no VM and no vfio-user socket.
ernic_add_test_program(test_ionic_port_info
    SOURCES
        test_ionic_port_info.c
        ${ERNIC_TEST_IONIC_ETH_SOURCES}
    DEPS glib
    HEADER_DEPS vfio-user ibverbs
)

# NIC profile loader test: ini layering, strict validation and atomic
# layers, built-in profiles by name and files by path. Pure: no ionic TU.
ernic_add_test_program(test_ionic_profile
    SOURCES
        test_ionic_profile.c
        ${PROJECT_SOURCE_DIR}/src/ionic/ionic_profile.c
        ${PROJECT_SOURCE_DIR}/src/ionic/ionic_profiles.c
    DEPS glib
)
target_compile_definitions(test_ionic_profile PRIVATE
    ERNIC_PROFILES_DIR="${PROJECT_SOURCE_DIR}/profiles")

# LIF counter block test: MAC classification, drop accounting, the
# little-endian rendering and each index against the kernel's struct
# ionic_lif_stats. Pure: no ionic TU.
ernic_add_test_program(test_ionic_emu_stats
    SOURCES
        test_ionic_emu_stats.c
        ${PROJECT_SOURCE_DIR}/src/ionic/ionic_emu_stats.c
)

# ionic NIC identity test. It links the eth emulator, calls its devcmd
# handlers through ionic_eth_emu_priv.h, checks the compat profile against
# golden bytes captured before profiles existed, and checks where each
# identity and link key lands.
ernic_add_test_program(test_ionic_identity
    SOURCES
        test_ionic_identity.c
        ${ERNIC_TEST_IONIC_ETH_SOURCES}
    DEPS glib
    HEADER_DEPS vfio-user ibverbs
)
target_include_directories(test_ionic_identity PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ionic statistics publish test. It links the eth emulator, reaches its state
# through ionic_eth_emu_priv.h, drives the datapath against a fake guest
# memory and checks what lands in the guest's struct ionic_lif_stats, and
# when.
ernic_add_test_program(test_ionic_stats_publish
    SOURCES
        test_ionic_stats_publish.c
        ${ERNIC_TEST_IONIC_ETH_SOURCES}
    DEPS glib
    HEADER_DEPS vfio-user ibverbs
)

# ionic queues test. It links the eth emulator, calls its devcmd handlers
# through ionic_eth_emu_priv.h, and checks the queue, EQ and vector counts
# each NIC profile advertises, and that queue indices past the count are
# refused.
ernic_add_test_program(test_ionic_queues
    SOURCES
        test_ionic_queues.c
        ${ERNIC_TEST_IONIC_ETH_SOURCES}
    DEPS glib
    HEADER_DEPS vfio-user ibverbs
)

# Valgrind's header, whose RUNNING_ON_VALGRIND lets a test skip what can't
# work under valgrind.
include(CheckIncludeFile)
check_include_file(valgrind/valgrind.h ERNIC_HAVE_VALGRIND_H)

# Server lifecycle test: forks the real server and drives it through
# startup, clients that drop out of vfio-user negotiation, and SIGTERM/SIGINT
# shutdown. Needs no symbols from the server, so it is a plain program. The
# server's path is compiled in rather than passed on the command line: a
# program that runs whatever its arguments name is what CodeQL's
# cpp/uncontrolled-process-operation reports.
ernic_add_test_program(test_server_lifecycle
    SOURCES
        test_server_lifecycle.c
        ${PROJECT_SOURCE_DIR}/src/ernic_gnu.c
)
target_compile_definitions(test_server_lifecycle PRIVATE
    "ERNIC_SERVER_PATH=\"$<TARGET_FILE:rocm-ernic>\"")
# A sanitizer runtime hangs at startup with RLIMIT_NOFILE below 4, and
# valgrind doesn't pass a lowered limit on to the kernel, so the server
# never gets it. The test skips the row that needs it in a sanitizer build
# or under valgrind; without valgrind's header, it can't tell, and the row
# runs.
if(ERNIC_USE_SANITIZERS OR ERNIC_USE_THREAD_SANITIZER)
    target_compile_definitions(test_server_lifecycle PRIVATE
        ERNIC_SERVER_SANITIZED)
endif()
if(ERNIC_HAVE_VALGRIND_H)
    target_compile_definitions(test_server_lifecycle PRIVATE
        ERNIC_HAVE_VALGRIND_H)
endif()
add_dependencies(test_server_lifecycle rocm-ernic)

# The vfio-user socket's permission helpers (ernic_socket.c), called
# directly on sockets in a scratch directory. The test fails the helpers'
# allocations, group lookups, chown(), chmod(), ACL lookup and unlink() on
# demand: the linker routes the calls in its own objects through wrappers,
# which pass every call they don't fail to the real function.
ernic_add_test_program(ernic-test-socket
    SOURCES
        test_ernic_socket.c
        ${PROJECT_SOURCE_DIR}/src/ernic_gnu.c
        ${PROJECT_SOURCE_DIR}/src/ernic_socket.c
        ${PROJECT_SOURCE_DIR}/src/ernic_log.c
)
target_link_options(ernic-test-socket PRIVATE
    "LINKER:--wrap=malloc,--wrap=strdup,--wrap=strndup"
    "LINKER:--wrap=getgrnam_r,--wrap=fchownat,--wrap=chmod"
    "LINKER:--wrap=lgetxattr,--wrap=unlinkat")

# ionic QUERY_QP test. It links the admin queue, calls
# ionic_adminq_dispatch_wqe() through ionic_adminq_priv.h, and backs the DMA
# entry points with a plain buffer, so the side buffers the driver reads its
# answer out of can be inspected directly.
ernic_add_test_program(test_ionic_query_qp
    SOURCES
        test_ionic_query_qp.c
        ${ERNIC_TEST_ADMINQ_SOURCES}
    HEADER_DEPS vfio-user glib ibverbs
)

# ionic hardware statistics test. It links the admin queue, calls
# ionic_adminq_dispatch_wqe() through ionic_adminq_priv.h, and backs DMA with
# a plain buffer, so the header table and values the driver builds
# hw_counters from can be checked.
ernic_add_test_program(ernic-test-ionic-stats
    SOURCES
        test_ionic_stats.c
        ${ERNIC_TEST_ADMINQ_SOURCES}
    HEADER_DEPS vfio-user glib ibverbs
)

# ionic admin queue poll test. It links the admin queue, stubs the rest with
# ionic_adminq_fakes.h, and writes WQEs into a ring in fake guest memory, so
# what ionic_adminq_poll() reads off the ring for each one can be checked.
ernic_add_test_program(ernic-test-ionic-adminq-poll
    SOURCES
        test_ionic_adminq_poll.c
        ${ERNIC_TEST_ADMINQ_SOURCES}
    HEADER_DEPS vfio-user glib ibverbs
)

# ionic admin queue id map test. It links the admin queue and stubs the rest
# with ionic_adminq_fakes.h, so the CQ, QP and MR maps can be filled
# directly, and checks a CREATE_CQ or CREATE_QP whose map is full, a
# CREATE_MR whose index has no slot, or any of them whose id is in use is
# refused before anything is allocated.
ernic_add_test_program(ernic-test-ionic-adminq-maps
    SOURCES
        test_ionic_adminq_maps.c
        ${ERNIC_TEST_ADMINQ_SOURCES}
    HEADER_DEPS vfio-user glib ibverbs
)

# ionic SRQ adminq unit test: drives CREATE/MODIFY/QUERY/DESTROY_SRQ through
# ionic_adminq_dispatch_wqe() against the same fake guest memory, so the
# response buffer AND the reported length can both be inspected -- the pair
# that was wrong while CREATE_SRQ wrote an id the driver was never told to
# read. Also pins the id-map capacity against the advertised max_srq.
ernic_add_test_program(test_ionic_srq_adminq
    SOURCES
        test_ionic_srq_adminq.c
        ${ERNIC_TEST_ADMINQ_SOURCES}
    HEADER_DEPS vfio-user glib ibverbs
)

# ionic fault-injection core test: the --inject parser, the deterministic
# patterns and the per-site fault set. Links the translation unit directly;
# it has no external dependencies to stub.
ernic_add_test_program(ernic-test-ionic-fault
    SOURCES
        test_ionic_fault.c
        ${PROJECT_SOURCE_DIR}/src/ionic/ionic_fault.c
)

# The checks ernic-rdma-errors makes on a guest's hw_counters
# (guest/ernic_counter_snapshot.h), run on the host. Header-only.
ernic_add_test_program(test_ionic_counter_snapshot
    SOURCES test_ionic_counter_snapshot.c
)

# ionic RDMA error-counter test. It links the datapath, reaches its state
# through ionic_datapath_priv.h, and fakes guest memory, the mesh and the
# clock, so each error path runs end to end with no vfio-user link and the
# RNR and pending-timeout rows sit exactly on their boundaries.
ernic_add_test_program(test_ionic_dp_errors
    SOURCES
        test_ionic_dp_errors.c
        ${ERNIC_TEST_DATAPATH_SOURCES}
    DEPS pthread
    HEADER_DEPS vfio-user glib ibverbs
)
# ERNIC_HAVE_P4 as the server has it, here and in the two tests below, so
# they test the data path that ships; each stubs the one P4 hook it calls.
# CodeQL also needs every build of ionic_datapath.c to agree: given two
# versions of a function, it calls one of them unreachable.
if(ERNIC_BUILD_P4)
    target_compile_definitions(test_ionic_dp_errors PRIVATE ERNIC_HAVE_P4)
endif()

# ionic CQ arm-semantics test. It links the datapath, reaches the private CQ
# state through ionic_datapath_priv.h, and counts the event callback
# in-process, so no guest memory is needed.
#
# libvfio-user supplies vfu_log/vfu_sgl_*; the arm path never reaches them, so
# the real library is cheaper than stubbing its ABI. Everything the datapath
# needs from the rest of this project is stubbed in the test itself.
ernic_add_test_program(test_ionic_cq_arm
    SOURCES
        test_ionic_cq_arm.c
        ${ERNIC_TEST_DATAPATH_SOURCES}
    DEPS vfio-user glib pthread
    HEADER_DEPS ibverbs
)
if(ERNIC_BUILD_P4)
    target_compile_definitions(test_ionic_cq_arm PRIVATE ERNIC_HAVE_P4)
endif()

# ionic receive-ring and buffer test. Like the CQ arm test, it links the
# datapath, reaches its rings through ionic_datapath_priv.h, and links the
# real libvfio-user rather than stubbing it.
ernic_add_test_program(test_ionic_rq_wrap
    SOURCES
        test_ionic_rq_wrap.c
        ${ERNIC_TEST_DATAPATH_SOURCES}
    DEPS vfio-user glib pthread
    HEADER_DEPS ibverbs
)
if(ERNIC_BUILD_P4)
    target_compile_definitions(test_ionic_rq_wrap PRIVATE ERNIC_HAVE_P4)
endif()

# TCP mesh broadcast test. It links the TCP backend, reaches its helpers
# through rdma_backend_tcp_priv.h, and stubs the backend's externals, so no
# RDMA subsystem link is needed; the broadcasts go over a socketpair.
ernic_add_test_program(test_tcp_mesh_topology
    SOURCES
        test_tcp_mesh_topology.c
        ${ERNIC_TCP_TEST_SOURCES}
    DEPS glib ibverbs pthread
    HEADER_DEPS vfio-user
)
# ASan's use-after-return detection would move the broadcast's frame off
# the poisoned stack the test gives it
ernic_disable_asan_use_after_return(test_tcp_mesh_topology)

# TCP GID -> node resolution test. Like the mesh-topology test it links the
# TCP backend, reaches its helpers through rdma_backend_tcp_priv.h, and stubs
# the backend's externals; resolution needs only the GID table, so no socket
# or thread is involved.
ernic_add_test_program(test_tcp_gid_resolve
    SOURCES
        test_tcp_gid_resolve.c
        ${ERNIC_TCP_TEST_SOURCES}
    DEPS glib ibverbs pthread
    HEADER_DEPS vfio-user
)

# RDMA backend handle test. It links the loopback and TCP backends, reaches
# the TCP backend's state through rdma_backend_tcp_priv.h, and stubs the
# backends' externals; creating and destroying objects needs no socket or
# thread.
ernic_add_test_program(ernic-test-backend-handles
    SOURCES
        test_rdma_backend_handles.c
        ${PROJECT_SOURCE_DIR}/src/rdma/rdma_backend_loopback.c
        ${ERNIC_TCP_TEST_SOURCES}
    DEPS glib ibverbs pthread
    HEADER_DEPS vfio-user
)

# TCP config test. Like the GID resolution test it links the TCP backend,
# reaches its helpers through rdma_backend_tcp_priv.h, and stubs the
# backend's externals; choosing a role needs no socket or thread.
ernic_add_test_program(ernic-test-tcp-config
    SOURCES
        test_tcp_config.c
        ${ERNIC_TCP_TEST_SOURCES}
    DEPS glib ibverbs pthread
    HEADER_DEPS vfio-user
)

# TCP thread stop test. It links the TCP backend and runs the real accept,
# health-check and receive threads on loopback sockets and socketpairs, with
# the thread entry points backed by pthreads, so a stop that never returns
# fails the test.
ernic_add_test_program(ernic-test-tcp-thread-stop
    SOURCES
        test_tcp_thread_stop.c
        ${ERNIC_TCP_TEST_SOURCES}
    DEPS glib ibverbs pthread
    HEADER_DEPS vfio-user
)
# The test skips a row that can't work under valgrind, which it detects
# with RUNNING_ON_VALGRIND from valgrind's header. Without the header, the
# row always runs.
if(ERNIC_HAVE_VALGRIND_H)
    target_compile_definitions(ernic-test-tcp-thread-stop PRIVATE
        ERNIC_HAVE_VALGRIND_H)
endif()
# The test fails the backend's malloc() calls on demand. The linker routes
# the calls in its own objects through a wrapper in the test; libc's and
# GLib's own calls are untouched.
target_link_options(ernic-test-tcp-thread-stop PRIVATE
    "LINKER:--wrap=malloc")

# RDMA resource manager test: the table allocator, which rdma_rm.c keeps
# static inline, so the test drives it through the resource manager's CQ
# table, and the other calls' paths, with backends whose operations fail on
# demand. It stubs the backend entry points rdma_rm.c calls, so no RDMA
# subsystem link is needed.
ernic_add_test_program(test_rdma_res_tbl
    SOURCES
        test_rdma_res_tbl.c
        ${PROJECT_SOURCE_DIR}/src/pvrdma/rdma_rm.c
        ${PROJECT_SOURCE_DIR}/src/qemu-compat/error-report.c
        ${PROJECT_SOURCE_DIR}/src/ernic_gnu.c
        ${PROJECT_SOURCE_DIR}/src/ernic_log.c
        ${PROJECT_SOURCE_DIR}/src/qemu-compat/qemu_stubs.c
    DEPS glib ibverbs pthread
)

# QEMU utility stubs test: bitmap word boundaries, the oversize-map
# abort, the signal mask of a new thread, and the mutex wrappers'
# aborts. It links the stubs translation unit directly.
ernic_add_test_program(test_qemu_stubs
    SOURCES
        test_qemu_stubs.c
        ${PROJECT_SOURCE_DIR}/src/ernic_gnu.c
        ${PROJECT_SOURCE_DIR}/src/qemu-compat/qemu_stubs.c
    DEPS glib ibverbs pthread
)
# TSan reports a misused mutex itself and halts, so the test skips the
# cases that misuse one.
if(ERNIC_USE_THREAD_SANITIZER)
    target_compile_definitions(test_qemu_stubs PRIVATE
        ERNIC_THREAD_SANITIZED)
endif()

# Log level test: ERNIC_LOG_LEVEL parsing, threads racing the first lookup,
# what each reporter prints, and the log limit. It links ernic_log.c and
# error-report.c directly.
ernic_add_test_program(ernic-test-log-level
    SOURCES
        test_log_level.c
        ${PROJECT_SOURCE_DIR}/src/qemu-compat/error-report.c
        ${PROJECT_SOURCE_DIR}/src/ernic_gnu.c
        ${PROJECT_SOURCE_DIR}/src/ernic_log.c
    DEPS pthread
)

# Statistics test: the counters, and the stats file ernic-exporter reads,
# which is replaced whole, never seen half written, and a failed write
# leaves the last one in place. It links ernic_stats.c and ernic_log.c
# directly, and fails their allocations on demand with the S3 tests'
# wrappers (s3_alloc_fail.h).
ernic_add_test_program(ernic-test-ernic-stats
    SOURCES
        test_ernic_stats.c
        s3_alloc_fail.c
        ${PROJECT_SOURCE_DIR}/src/ernic_stats.c
        ${PROJECT_SOURCE_DIR}/src/ernic_log.c
    DEPS glib pthread
)
target_link_options(ernic-test-ernic-stats PRIVATE
    "LINKER:--wrap=malloc,--wrap=calloc,--wrap=realloc"
    "LINKER:--wrap=strdup,--wrap=getentropy")

# In-process ionic device harness. It links every source the server is
# built from except SERVER_SOURCES (main() and the socket's permissions),
# with fake_vfu.c standing in for libvfio-user, and ionic_harness.c drives
# the device the way the guest driver does: registers, doorbells, and rings
# in fake guest memory. No VM, socket or RDMA device is involved, so it runs
# in every build leg. The source lists come from the top-level
# CMakeLists.txt, so the harness keeps linking what the server links.
set(ERNIC_HARNESS_DEVICE_SOURCES
    ${COMMON_SOURCES}
    ${BRIDGE_SOURCES}
    ${IONIC_EMU_SOURCES}
    ${NVMEOF_SOURCES}
    ${S3_SOURCES}
    ${RDMA_BACKEND_SOURCES}
    ${QEMU_COMPAT_SOURCES}
    ${PVRDMA_SOURCES}
)
# No libvfio-user: fake_vfu.c defines the entry points the device calls.
set(ERNIC_HARNESS_DEPS glib ibverbs pthread)
if(ERNIC_BUILD_P4)
    list(APPEND ERNIC_HARNESS_DEVICE_SOURCES ${P4_SOURCES})
    list(APPEND ERNIC_HARNESS_DEPS json-c)
endif()
list(TRANSFORM ERNIC_HARNESS_DEVICE_SOURCES PREPEND "${PROJECT_SOURCE_DIR}/")
ernic_add_test_program(ernic-test-ionic-harness
    SOURCES
        test_ionic_harness.c
        ionic_harness.c
        fake_vfu.c
        s3_alloc_fail.c
        ${ERNIC_HARNESS_DEVICE_SOURCES}
    DEPS ${ERNIC_HARNESS_DEPS}
    HEADER_DEPS vfio-user
)
# The fp8 groups fail the data path's allocations on demand with the S3
# tests' wrappers (s3_alloc_fail.h). Unarmed, they pass every call
# through, so the other groups see the real allocator.
target_link_options(ernic-test-ionic-harness PRIVATE
    "LINKER:--wrap=malloc,--wrap=calloc,--wrap=realloc"
    "LINKER:--wrap=strdup,--wrap=getentropy")
if(ERNIC_BUILD_P4)
    target_compile_definitions(ernic-test-ionic-harness PRIVATE
        ERNIC_HAVE_P4
        P4_FIXTURE_DIR="${ERNIC_TEST_P4_FIXTURE_DIR}")
endif()

# Two whole devices in one process, meshed over loopback TCP: the GSI/CM
# path that rdma_cm depends on, which test_ud_mesh can only cover with two
# VMs. Same sources as the harness above -- it is the harness, twice.
ernic_add_test_program(ernic-test-gsi-mesh
    SOURCES
        test_gsi_mesh.c
        ionic_harness.c
        fake_vfu.c
        ${ERNIC_HARNESS_DEVICE_SOURCES}
    DEPS ${ERNIC_HARNESS_DEPS}
    HEADER_DEPS vfio-user
)
# It builds ERNIC_HARNESS_DEVICE_SOURCES, which carries the P4 engine
# when ERNIC_BUILD_P4 is on, so it needs the same define and the same
# json-c as the harness target beside it.
if(ERNIC_BUILD_P4)
    target_compile_definitions(ernic-test-gsi-mesh PRIVATE ERNIC_HAVE_P4)
endif()

# ---------------------------------------------------------------
# Register tests with CTest
# ---------------------------------------------------------------

# PCI config test uses run-test.sh to start the server, run the
# test client, and clean up.
add_test(
    NAME pci-config-test
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run-test.sh
        $<TARGET_FILE:test_pci_client>
        $<TARGET_FILE:rocm-ernic>
)
set_tests_properties(pci-config-test PROPERTIES
    TIMEOUT 30
    RUN_SERIAL TRUE
)

# RDMA device discovery must match the complete emulated PCI tuple, not
# a physical Pensando function with a shared vendor or device ID.
add_test(
    NAME find-rdma-device-tuple
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_find_rdma_device.sh
)
set_tests_properties(find-rdma-device-tuple PROPERTIES TIMEOUT 30)

# The VM tests look up each guest's IPv4-mapped GID index rather than
# assuming one.
add_test(
    NAME find-ipv4-gid-index
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_find_ipv4_gid_index.sh
)
set_tests_properties(find-ipv4-gid-index PROPERTIES TIMEOUT 30)

ernic_add_unit_test(ernic-device-tuple-unit test_ernic_device
    TIMEOUT 30 RUN_SERIAL)

# test_data_transfer, test_write_imm and test_rdma_cm -- and the guest-only
# SRQ and UD tests built alongside them -- are deliberately not
# registered with CTest. They need a real RDMA device, which no host running
# ctest has -- the runners run ctest outside the guest. Registered here they
# returned 77 and CTest reported the skip as a pass, so they looked like
# coverage while testing nothing. They are built (test_data_transfer is the
# loopback config step's TEST_BIN) and are run for real inside the VM by the
# loopback job in system-tests.yml, where a device is guaranteed and exit 77
# is treated as a failure.

# Guest DMA range check unit test (table-driven)
ernic_add_unit_test(dma-range-unit test_dma_range TIMEOUT 30)

ernic_add_unit_test(ernic-dma-unit ernic-test-dma TIMEOUT 30)

# BAR0 access classification (table-driven)
ernic_add_unit_test(ionic-bar-unit ernic-test-ionic-bar TIMEOUT 30)

# TAP backend: an overlong interface name is refused, not truncated
ernic_add_unit_test(ionic-eth-net-unit ernic-test-ionic-eth-net TIMEOUT 30)

# Strict integer parser unit test (table-driven)
ernic_add_unit_test(parse-int-unit test_parse_int TIMEOUT 30)

# Byte-order accessors against hand-written byte images
ernic_add_unit_test(byteorder-unit ernic-test-byteorder TIMEOUT 30)

# SipHash-2-4 against the reference implementation's test vectors
ernic_add_unit_test(siphash-unit ernic-test-siphash TIMEOUT 30)

# P4 engine unit test: port model, frame bounds, and the no-program path.
if(ERNIC_BUILD_P4)
    # Loads five programs padded to the parse budget, each of which has
    # json-c build a tree of 36 to 52 MiB in small allocations. That takes
    # about a second, but over 30 s under valgrind.
    ernic_add_unit_test(p4-engine-unit ernic-test-p4-engine TIMEOUT 120)

    ernic_add_unit_test(p4-alloc-fail-unit ernic-test-p4-alloc-fail TIMEOUT 30)

    ernic_add_unit_test(p4-fp8-unit ernic-test-p4-fp8 TIMEOUT 30)

    # Sweeps every encoding of every format and a few million floats, so
    # it gets longer than the 30 s the other unit tests take.
    ernic_add_unit_test(p4-fmt-unit ernic-test-p4-fmt TIMEOUT 120)
endif()

# NVMe-oF controller unit test: drives the capsule sequence a real
# nvme-rdma initiator emits, from Fabrics Connect to read-after-write.
ernic_add_unit_test(nvmeof-target-unit test_nvmeof_target TIMEOUT 30)

# IB CM responder unit test: REQ/REP/RTU/DREQ, plus every rejection path
# a misaddressed or mismatched "nvme connect" can take.
ernic_add_unit_test(nvmeof-cm-unit test_nvmeof_cm TIMEOUT 30)

# S3-over-RDMA token codec unit test: the x-amz-rdma-token and
# x-amz-rdma-reply wire formats, pinned against a hand-assembled image.
ernic_add_unit_test(s3-token-unit test_s3_token TIMEOUT 30)

# S3 control-plane HTTP unit test: request parsing including incremental
# arrival, query handling, and the response builder.
ernic_add_unit_test(s3-http-unit test_s3_http TIMEOUT 30)

# S3 object store unit test: PUT, GET, ranged GET, HEAD, DELETE,
# ListObjectsV2 and the multipart upload sequence, over a fake DMA.
ernic_add_unit_test(s3-target-unit test_s3_target TIMEOUT 60)

# In-band TCP endpoint unit test: ARP, the handshake, split segments,
# keep-alive and teardown, driven with raw Ethernet frames.
ernic_add_unit_test(s3-tcp-unit test_s3_tcp TIMEOUT 60)

# ionic MSI-X mask/pending unit test: asserts an assertion raised on a masked
# vector is latched and replayed on unmask rather than dropped.
ernic_add_unit_test(ionic-intr-pending-unit test_ionic_intr_pending TIMEOUT 30)

# ionic PORT_INIT unit test: asserts the port-info block the driver reads is
# actually published. While PORT_INIT was a stub it stayed zeroed, the
# transceiver read back as absent, ionic_get_link_ksettings() resolved
# PORT_NONE and never assigned a speed -- so ethtool reported "Unknown!" and
# UCX refused to build an endpoint on a device with bandwidth 0.
ernic_add_unit_test(ionic-port-info-unit test_ionic_port_info TIMEOUT 30)

# Server lifecycle: a client that hangs up or sends garbage during vfio-user
# negotiation must not kill the server, and SIGTERM/SIGINT must reach a clean
# "Shutdown complete". When the server is built with ERNIC_USE_SANITIZERS,
# LeakSanitizer runs at its exit, so this is also the ionic teardown leak
# check. One server per row, each on its own socket.
add_test(
    NAME server-lifecycle
    COMMAND $<TARGET_FILE:test_server_lifecycle>
)
set_tests_properties(server-lifecycle PROPERTIES
    TIMEOUT 300
)

# The socket's permission helpers, each refusal checked for its reason,
# including the paths the server can't be driven down
ernic_add_unit_test(ernic-socket-unit ernic-test-socket TIMEOUT 30)

# NIC profile loader unit test: asserts strict validation (every error names
# the source, line and key), atomic layers, and that the built-in profiles
# match the files shipped in profiles/.
ernic_add_unit_test(ionic-profile-unit test_ionic_profile TIMEOUT 30)

# LIF counter block unit test: a counter in the wrong qword shows up as the
# wrong `ip -s link` field in the guest, so every index is pinned.
ernic_add_unit_test(ionic-emu-stats-unit test_ionic_emu_stats TIMEOUT 30)

# ionic NIC identity unit test: asserts compat is byte-identical to the
# device before profiles, and each identity/link key reaches the field the
# driver reads.
ernic_add_unit_test(ionic-identity-unit test_ionic_identity TIMEOUT 30)

# ionic statistics publish unit test: when the LIF block is rewritten in guest
# memory, where, and what the datapath counted into it.
ernic_add_unit_test(ionic-stats-publish-unit test_ionic_stats_publish
    TIMEOUT 30)

# ionic queues unit test: the queue, EQ and MSI-X vector counts each NIC
# profile advertises, and whether the driver's vector budget fits them.
ernic_add_unit_test(ionic-queues-unit test_ionic_queues TIMEOUT 30)

# ionic QUERY_QP unit test: asserts the command writes the QP's real state
# into the side buffers the driver reads, rather than leaving them zeroed and
# reporting every queue pair as RESET.
ernic_add_unit_test(ionic-query-qp-unit test_ionic_query_qp TIMEOUT 30)

# ionic hardware statistics unit test: asserts the four stats admin commands
# answer with the header table and LE64 values the driver turns into its
# hw_counters files, clamped to the buffer, and fail on an unknown QP.
ernic_add_unit_test(ionic-stats-unit ernic-test-ionic-stats TIMEOUT 30)

# ionic admin queue poll unit test: asserts a WQE runs only on bytes read
# from its own ring entries, so one claiming more strides than were produced
# or than fit is refused, and one that wraps is finished from slot 0.
ernic_add_unit_test(ionic-adminq-poll-unit ernic-test-ionic-adminq-poll
    TIMEOUT 30)

# ionic admin queue id map unit test: a CREATE_CQ, CREATE_QP or CREATE_MR
# whose map is full or whose id is in use is refused, and allocates nothing
# that could never be freed.
ernic_add_unit_test(ionic-adminq-maps-unit ernic-test-ionic-adminq-maps
    TIMEOUT 30)

# ionic SRQ adminq unit test: the four SRQ handlers had no in-process
# coverage, which is how CREATE_SRQ shipped writing its id into a response
# the driver was told was zero bytes long.
ernic_add_unit_test(ionic-srq-adminq-unit test_ionic_srq_adminq TIMEOUT 30)

# ionic fault-injection unit test: asserts each --inject pattern fires on
# exactly the events it names, malformed specs are rejected naming the bad
# token, and the canonical form parses back to the same fault.
ernic_add_unit_test(ionic-fault-unit ernic-test-ionic-fault TIMEOUT 30)

# hw_counters snapshot unit test: a sysfs counter's text is read only as
# plain unsigned decimal, the counter names before and after a run must be
# the same set, and a counter that went down is refused rather than read
# as a huge increase.
ernic_add_unit_test(ionic-counter-snapshot-unit test_ionic_counter_snapshot
    TIMEOUT 30)

# ionic CQ arm unit test: asserts arming a CQ that still holds unreaped CQEs
# raises the event immediately, rather than waiting for a CQE that never comes.
ernic_add_unit_test(ionic-cq-arm-unit test_ionic_cq_arm TIMEOUT 30)

# ionic RDMA error-counter unit test: asserts each RDMA error the data path
# hands the guest moves exactly the hw_counters it names, on the device and
# on the QP involved, and nothing else.
ernic_add_unit_test(ionic-dp-errors-unit test_ionic_dp_errors TIMEOUT 30)

# ionic receive-ring unit test: the doorbell's producer index is masked to the
# ring by the driver, so reading it as a free-running counter broke on the
# first wrap and reported tens of thousands of receives outstanding.
ernic_add_unit_test(ionic-rq-wrap-unit test_ionic_rq_wrap TIMEOUT 30)

# TCP mesh-topology broadcast unit test: asserts the unused node slots in the
# fixed-size payload are zeroed rather than carrying stack contents, and that
# the topology and GID broadcasts report a mesh table past their node cap.
ernic_add_unit_test(tcp-mesh-topology-unit test_tcp_mesh_topology
    TIMEOUT 30 RUN_SERIAL)

# TCP GID -> node resolution unit test: exact 16-byte matching, no-match
# failure, the zero-filled advertisement tail, and the mesh entry points
# leaving a non-TCP backend's private block alone.
ernic_add_unit_test(tcp-gid-resolve-unit test_tcp_gid_resolve TIMEOUT 30)

# RDMA backend handle unit test: asserts each loopback device numbers its MR
# lkeys from 1, TCP QPNs skip the special QPs and live QPs after wrapping,
# and both backends' QPs and SRQs keep their state through every operation.
ernic_add_unit_test(rdma-backend-handles-unit ernic-test-backend-handles
    TIMEOUT 30)

# TCP config unit test: asserts the manager or worker role is read from the
# config's prefix, so a worker whose manager's host name contains
# "manager:" stays a worker, and that a config naming no role is refused.
ernic_add_unit_test(tcp-config-unit ernic-test-tcp-config TIMEOUT 30)

# TCP thread stop unit test: asserts stopping the accept thread returns while
# it is blocked in accept(), stopping the health-check thread returns without
# waiting out its interval, every receive thread is stopped and joined once,
# and tcp_fini() returns while a manager thread is blocked sending to a worker
# that stopped reading, which is what lets tcp_fini() finish on SIGTERM
# promptly and free the backend with no thread still using it.
#
# The test spends most of its time waiting on purpose: about 50 s, and 90 to
# 120 s under valgrind in CI, which runs a program's threads one at a time
# and shares the runner's CPUs with the other tests. Each row's alarm() still
# turns a hang into a named failure long before this.
ernic_add_unit_test(tcp-thread-stop-unit ernic-test-tcp-thread-stop
    TIMEOUT 300)

# RDMA resource-table allocator unit test: asserts a full table refuses the
# next allocation rather than handing back a handle one past the end.
ernic_add_unit_test(rdma-res-tbl-unit test_rdma_res_tbl TIMEOUT 30 RUN_SERIAL)

# QEMU utility stubs unit test (bitmap, thread signal mask)
ernic_add_unit_test(qemu-stubs-unit test_qemu_stubs TIMEOUT 30)

# Log level unit test: ERNIC_LOG_LEVEL, and the race on its first lookup
ernic_add_unit_test(log-level-unit ernic-test-log-level TIMEOUT 60)

# Statistics unit test: the counters, and the stats file replaced whole and
# kept when a write fails
ernic_add_unit_test(ernic-stats-unit ernic-test-ernic-stats TIMEOUT 30)

# In-process ionic device harness: the whole device, driven as the guest
# driver drives it, one CTest test per group so a failure names its area.
# Bring-up and devcmds, RDMA_RESET_LIF, refused admin commands, RC
# transfers, atomics and fast registration, CQ events, UD, SRQ (srq-*-ring
# with each provider's doorbell; srq with the kernel's), Ethernet
# Tx/Rx, the NVMe-oF CM handshake and Connect over the GSI QP, the S3
# store's RDMA GET and PUT, key 0 refused by both targets, and the flush a
# QP entering the Error state owes its outstanding work requests.
# Also which --backend strings name which backend, a QP's life on the none
# backend, the loopback backend pairing QPs that name no destination, and
# RDMA_RESET_LIF freeing everything the driver made. The *-dma groups refuse
# one DMA at a time in the admin queue, the data path and Ethernet,
# tables fills the EQ, admin queue and MR tables, and wrap sends lengths
# and registers regions whose sums or ends carry past 2^32 or 2^64.
# spec-sge moves data with work requests laid out behind a speculative
# SGE length table, as the driver posts them, and refuses list formats it
# doesn't define. mr-keys checks that only the key an MR is bound under
# reaches it, through fast registration's rotations, LOCAL_INV and
# DESTROY_MR.
# limits checks that the counts the LIF identity promises are what every
# backend's tables hold, and fills the CQ and QP ids to the last.
# nvmeof-queues refuses more NVMe-oF queues than the MR table can serve.
# rdma-reset-faults checks that the admin queue made again after
# RDMA_RESET_LIF still injects the --inject faults. mesh-none checks that
# only the TCP backend has a mesh, and that the mesh calls on any other
# backend answer without one.
set(ERNIC_HARNESS_GROUPS
    devcmds bringup rdma-reset rdma-reset-faults rdma-reset-frees admin
    admin-dma rc spec-sge atomic wrap mr-keys dp-dma tables limits cq-events
    qp-flush qp-flush-srq ud srq srq-rq-ring srq-cq-ring eth eth-dma
    nvmeof nvmeof-queues s3 mesh-none teardown backend-names none
    loopback-pair)
if(ERNIC_BUILD_P4)
    list(APPEND ERNIC_HARNESS_GROUPS p4-port p4-attach fp8-map fp8-overlap
                                      p4-rdma p4-cvt p4-cvt-map p4-drop-map
                                      p4-deny p4-log-limit p4-devcmd
                                      p4-devcmd-errors)
endif()
foreach(ernic_harness_group IN LISTS ERNIC_HARNESS_GROUPS)
    ernic_add_unit_test(ionic-harness-${ernic_harness_group}
        ernic-test-ionic-harness
        TIMEOUT 60
        ARGS ${ernic_harness_group})
endforeach()

# The GSI/CM mesh path: two devices in one process over loopback TCP.
# RUN_SERIAL because it binds a fixed port; ERNIC_TEST_MESH_PORT moves it.
#
# 180s, not the 60 the harness groups use: this one waits on a real TCP
# registration handshake between two devices, and the memcheck lane runs it
# under valgrind, where that handshake is far slower than the work itself.
ernic_add_unit_test(gsi-mesh ernic-test-gsi-mesh TIMEOUT 180 RUN_SERIAL)

# ionic server smoke test: start the server, verify socket and banner.
# Tests that the ionic emulation layer initialises without crashing.
add_test(
    NAME ionic-server-smoke
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_ionic_server_start.sh
        $<TARGET_FILE:rocm-ernic>
)
set_tests_properties(ionic-server-smoke PROPERTIES
    TIMEOUT 15
    RUN_SERIAL TRUE
)

# --stats-interval-ms range checks and precedence over --nic-profile, read
# back through --print-profile.  The server exits before opening a socket.
add_test(
    NAME ionic-stats-interval-cli
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_ionic_stats_interval_cli.sh
        $<TARGET_FILE:rocm-ernic>
)
set_tests_properties(ionic-stats-interval-cli PROPERTIES
    TIMEOUT 30
)

# --eth-queues range checks and precedence over --nic-profile, read back
# through --print-profile.  The server exits before opening a socket.
add_test(
    NAME ionic-eth-queues-cli
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_ionic_eth_queues_cli.sh
        $<TARGET_FILE:rocm-ernic>
)
set_tests_properties(ionic-eth-queues-cli PROPERTIES
    TIMEOUT 30
)

# ionic CI test suite: comprehensive checks on the ionic emulation path
# (VID:DID, BAR layout, MSI-X vectors, multi-backend startup, clean shutdown).
# Does not require a VM or RDMA hardware.
add_test(
    NAME ionic-ci
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_ionic_ci.sh
        $<TARGET_FILE:rocm-ernic>
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
)
set_tests_properties(ionic-ci PROPERTIES
    TIMEOUT 60
    RUN_SERIAL TRUE
)

# TCP mesh registration suite: GID advertisement across a three-node mesh,
# an unconfigured node, a contested GID, and the protocol version gate in
# both directions. Drives real servers over loopback TCP; no VM or RDMA
# hardware needed. RUN_SERIAL because it starts several servers at once.
add_test(
    NAME tcp-mesh-ci
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_tcp_mesh_ci.sh
        $<TARGET_FILE:rocm-ernic>
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
)
# Generous timeout: the suite starts several servers and waits on mesh
# convergence, and runs in every build leg including thread-sanitizer
# Release, where that chain is slow. The script's own budgets are the real
# bound; this only has to be larger than them.
set_tests_properties(tcp-mesh-ci PROPERTIES
    TIMEOUT 420
    RUN_SERIAL TRUE
)

# NVMe-oF backend CI suite: every documented option spelling starts a server
# with the namespace it asked for, a file-backed namespace appears on disk at
# the right size, and bad options are refused before startup.
# Does not require a VM or RDMA hardware.
add_test(
    NAME nvmeof-ci
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_nvmeof_ci.sh
        $<TARGET_FILE:rocm-ernic>
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
)
set_tests_properties(nvmeof-ci PROPERTIES
    TIMEOUT 120
    RUN_SERIAL TRUE
)

# S3 backend CI suite: every documented option spelling starts a server with
# the store it asked for, the in-band endpoint announces the address and MAC
# the guest will ARP for, and bad options are refused before startup.
# Does not require a VM or RDMA hardware.
add_test(
    NAME s3-ci
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_s3_ci.sh
        $<TARGET_FILE:rocm-ernic>
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
)
set_tests_properties(s3-ci PROPERTIES
    TIMEOUT 120
    RUN_SERIAL TRUE
)

# ---------------------------------------------------------------
# Python suites that drive real servers over the vfio-user socket
# ---------------------------------------------------------------
# tests/py/ stands in for a guest VM: it speaks the vfio-user protocol to
# a real rocm-ernic process, maps guest memory from a memfd the server
# maps too, and plays the ionic driver against it. None of it needs a VM,
# RDMA hardware, or root. See docs/testing.rst.
#
# REQUIRED: python3 is already a run-time dependency of the shell suites
# (test_tcp_mesh_ci.sh and the CI job scripts), so a host that builds the
# tests without it cannot run them. Registering these only when it happens
# to be found would let CI pass with three fewer tests and no sign of it,
# and a skipped or missing test is exactly what must not pass quietly.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# -I keeps the interpreter from picking up anything outside the tree: the
# suites put tests/py on sys.path themselves.
set(ERNIC_PY_RUN ${Python3_EXECUTABLE} -I)

# The vfio-user protocol itself: version negotiation, regions, DMA
# mapping, interrupts, and the offsets, counts and ranges a client
# must not get away with.
add_test(
    NAME ernic-vfu-client
    COMMAND ${ERNIC_PY_RUN}
        ${CMAKE_CURRENT_SOURCE_DIR}/py/test_vfu_client.py
        --server $<TARGET_FILE:rocm-ernic>
)
set_tests_properties(ernic-vfu-client PROPERTIES
    TIMEOUT 180
    RUN_SERIAL TRUE
)

# The ionic driver over that protocol: bring-up, queue pairs, memory
# regions, work requests and completions, through a server process
# rather than the in-process harness in ionic_harness.c.
add_test(
    NAME ernic-ionic-driver
    COMMAND ${ERNIC_PY_RUN}
        ${CMAKE_CURRENT_SOURCE_DIR}/py/test_ionic_driver.py
        --server $<TARGET_FILE:rocm-ernic>
)
set_tests_properties(ernic-ionic-driver PROPERTIES
    TIMEOUT 300
    RUN_SERIAL TRUE
)

# The P4 engine on the live device: guest frames out of the Ethernet Tx
# ring, through the pipeline, and out the far side. Starts a server per
# case because the statistics it asserts on are reported at shutdown.
if(ERNIC_BUILD_P4)
    add_test(
        NAME ernic-p4-driver
        COMMAND ${ERNIC_PY_RUN}
            ${CMAKE_CURRENT_SOURCE_DIR}/py/test_p4.py
            --server $<TARGET_FILE:rocm-ernic>
    )
    set_tests_properties(ernic-p4-driver PROPERTIES
        TIMEOUT 300
        RUN_SERIAL TRUE
    )
endif()

# The hardware statistics a guest's hw_counters are built from: the
# STATS_HDRS / STATS_VALS / QP_STATS_HDRS / QP_STATS_VALS tables, the
# values behind them, and that the --stats-file the exporter reads
# carries the same numbers.
add_test(
    NAME ernic-ionic-hw-stats
    COMMAND ${ERNIC_PY_RUN}
        ${CMAKE_CURRENT_SOURCE_DIR}/py/test_ionic_hw_stats.py
        --server $<TARGET_FILE:rocm-ernic>
)
set_tests_properties(ernic-ionic-hw-stats PROPERTIES
    TIMEOUT 300
    RUN_SERIAL TRUE
)

# A three-node mesh under traffic, malformed peers, and churn.
# Generous timeout: it starts and restarts several servers and waits
# on mesh convergence, in every build leg including thread-sanitizer
# Release, where that chain is slow.
add_test(
    NAME ernic-mesh-stress
    COMMAND ${ERNIC_PY_RUN}
        ${CMAKE_CURRENT_SOURCE_DIR}/py/test_mesh_stress.py
        --server $<TARGET_FILE:rocm-ernic>
)
set_tests_properties(ernic-mesh-stress PROPERTIES
    TIMEOUT 600
    RUN_SERIAL TRUE
)

# A three-node mesh pushed past what a node queues from its peers: two
# nodes writing to each other flat out, and a peer that floods a node.
# Serial: it moves over a gigabyte between the nodes.
add_test(
    NAME ernic-mesh-flow
    COMMAND ${ERNIC_PY_RUN}
        ${CMAKE_CURRENT_SOURCE_DIR}/py/test_mesh_flow.py
        --server $<TARGET_FILE:rocm-ernic>
)
set_tests_properties(ernic-mesh-flow PROPERTIES
    TIMEOUT 600
    RUN_SERIAL TRUE
)

# ---------------------------------------------------------------
# Compile checks for the Nix fuzz harnesses
# ---------------------------------------------------------------
# Builds nix/analysis/fuzz/ against a plain main() so the harnesses cannot
# go stale between opt-in fuzz runs. See tests/nix/CMakeLists.txt.
add_subdirectory(nix)

# ---------------------------------------------------------------
# Build as a subproject of another CMake project
# ---------------------------------------------------------------
add_subdirectory(subproject)

# ---------------------------------------------------------------
# Build and check the DEB packages
# ---------------------------------------------------------------
add_subdirectory(package)

# ---------------------------------------------------------------
# One language and C library interface for every file
# ---------------------------------------------------------------
# Every file is compiled as exactly the C standard CMAKE_C_STANDARD names,
# with the feature-test macros ernic_set_compiler_flags() defines and no
# others, except src/ernic_gnu.c, the one file allowed _GNU_SOURCE. The
# check reads compile_commands.json, which only the Ninja and Makefile
# generators write, at the top of the build tree: CMAKE_BINARY_DIR, which
# is a superproject's when rocm-ernic is built inside one.
set(ERNIC_STDC_VERSION_99 199901L)
set(ERNIC_STDC_VERSION_11 201112L)
set(ERNIC_STDC_VERSION_17 201710L)
if(NOT DEFINED ERNIC_STDC_VERSION_${CMAKE_C_STANDARD})
    message(FATAL_ERROR "No __STDC_VERSION__ for C${CMAKE_C_STANDARD}; add "
      "it to tests/CMakeLists.txt")
endif()
if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
    add_test(
        NAME ernic-feature-macros
        COMMAND ${CMAKE_COMMAND}
            "-DCOMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DBINARY_DIR=${PROJECT_BINARY_DIR}"
            "-DGNU_SOURCE_FILE=${PROJECT_SOURCE_DIR}/src/ernic_gnu.c"
            "-DSTDC_VERSION=${ERNIC_STDC_VERSION_${CMAKE_C_STANDARD}}"
            -P ${PROJECT_SOURCE_DIR}/cmake/ErnicCheckFeatureMacros.cmake
    )
    set_tests_properties(ernic-feature-macros PROPERTIES TIMEOUT 300)
endif()
