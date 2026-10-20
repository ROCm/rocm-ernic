# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Tests build configuration

# -- test_pci_client (no extra dependencies) ---------------------
add_executable(test_pci_client
    test_pci_client.c
)
set_target_properties(test_pci_client PROPERTIES
    EXCLUDE_FROM_ALL FALSE
)

# -- test_data_transfer (requires libibverbs) --------------------
add_executable(test_data_transfer
    test_data_transfer.c
)
target_compile_definitions(test_data_transfer PRIVATE
    _GNU_SOURCE
)
target_include_directories(test_data_transfer PRIVATE
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_link_directories(test_data_transfer PRIVATE
    ${IBVERBS_LIBRARY_DIRS}
)
target_link_libraries(test_data_transfer PRIVATE
    ${IBVERBS_LIBRARIES}
)

# -- test_rdma_cm (requires libibverbs) --------------------------
add_executable(test_rdma_cm
    test_rdma_cm.c
)
target_include_directories(test_rdma_cm PRIVATE
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_link_directories(test_rdma_cm PRIVATE
    ${IBVERBS_LIBRARY_DIRS}
)
target_link_libraries(test_rdma_cm PRIVATE
    ${IBVERBS_LIBRARIES}
)

# -- Guest-only SRQ and UD tests (require libibverbs) ------------
# Built here, never registered with CTest, for the reason the comment
# further down gives for test_data_transfer and test_rdma_cm: they need a
# real RDMA device, and on a host without one they exit 77, which CTest
# scores as a pass. They run for real in the guest, from system-tests.yml.
#
# Building them is still worth it. Until they were added here they were in
# no target at all, so a syntax or type error in any of them surfaced only
# when gcc ran inside the VM, well into a 60-minute self-hosted job. Now
# the hosted build legs catch it in seconds, under the same warning and
# hardening flags as every other test target.
set(ERNIC_GUEST_ONLY_TESTS
    test_srq
    test_srq_transfer
    test_ud_transfer
    test_ud_sq_reclaim
    test_ud_mesh
)
foreach(guest_test ${ERNIC_GUEST_ONLY_TESTS})
    add_executable(${guest_test} ${guest_test}.c)
    target_compile_definitions(${guest_test} PRIVATE _GNU_SOURCE)
    target_include_directories(${guest_test} PRIVATE
        ${ERNIC_IBVERBS_INCLUDE_DIRS}
    )
    target_link_directories(${guest_test} PRIVATE ${IBVERBS_LIBRARY_DIRS})
    target_link_libraries(${guest_test} PRIVATE ${IBVERBS_LIBRARIES})
endforeach()

# -- Unit tests --------------------------------------------------

# Vendored QEMU sources that tests compile as separate translation units.
# Suppressing warnings per-source rather than per-target keeps the vendored
# C quiet (matching the main build) while the test's own source is still
# warning-checked -- a target-wide -w would disable warnings everywhere,
# since GCC honours -w regardless of where it appears in the flag list.
set(ERNIC_TEST_VENDORED_SOURCES
    ${PROJECT_SOURCE_DIR}/third-party/qemu/hw/rdma/rdma_rm.c
)
set_source_files_properties(${ERNIC_TEST_VENDORED_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-w"
)

# -- test_write_imm (requires libibverbs) ------------------------
add_executable(test_write_imm
    test_write_imm.c
)
target_compile_definitions(test_write_imm PRIVATE
    _POSIX_C_SOURCE=200809L
)
target_include_directories(test_write_imm PRIVATE
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_link_directories(test_write_imm PRIVATE
    ${IBVERBS_LIBRARY_DIRS}
)
target_link_libraries(test_write_imm PRIVATE
    ${IBVERBS_LIBRARIES}
)

# Fixture-backed tests for the C-side emulated RDMA device lookup.
add_executable(test_ernic_device test_ernic_device.c)
target_compile_definitions(test_ernic_device PRIVATE
    _POSIX_C_SOURCE=200809L
    ERNIC_SYSFS_INFINIBAND="${CMAKE_CURRENT_BINARY_DIR}/ernic-device-fixture/class/infiniband"
    ERNIC_TEST_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/ernic-device-fixture"
)
target_include_directories(test_ernic_device PRIVATE
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)

# Strict integer parser (parse_int.h) unit test (header-only).
add_executable(test_parse_int test_parse_int.c)
target_include_directories(test_parse_int PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

# NVMe-oF controller test. The controller core takes its DMA through a
# callback pair, so the test links the real translation unit and stands a
# byte array in for guest memory -- no VM, no RDMA device, no server.
add_executable(test_nvmeof_target
    test_nvmeof_target.c
    ${PROJECT_SOURCE_DIR}/src/nvmeof_target.c
)
target_include_directories(test_nvmeof_target PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_include_directories(test_nvmeof_target SYSTEM PRIVATE
    ${VFIO_USER_INCLUDE_DIRS}
)
target_compile_definitions(test_nvmeof_target PRIVATE _GNU_SOURCE)

# IB CM responder test. Feeds it the CM MADs a guest running rdma_cm
# emits and dissects the replies; no MAD ever reaches a wire.
add_executable(test_nvmeof_cm
    test_nvmeof_cm.c
    ${PROJECT_SOURCE_DIR}/src/nvmeof_cm.c
    ${PROJECT_SOURCE_DIR}/src/nvmeof_target.c
)
target_include_directories(test_nvmeof_cm PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_include_directories(test_nvmeof_cm SYSTEM PRIVATE
    ${VFIO_USER_INCLUDE_DIRS}
)
target_compile_definitions(test_nvmeof_cm PRIVATE _GNU_SOURCE)

# S3-over-RDMA token codec test. The encoding is a wire contract shared
# with hipObject, so this links nothing but the codec itself.
add_executable(test_s3_token
    test_s3_token.c
    ${PROJECT_SOURCE_DIR}/src/s3_token.c
)
target_include_directories(test_s3_token PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_s3_token PRIVATE _GNU_SOURCE)

# S3 control-plane HTTP subset test: buffers in, buffers out, no socket.
add_executable(test_s3_http
    test_s3_http.c
    ${PROJECT_SOURCE_DIR}/src/s3_http.c
)
target_include_directories(test_s3_http PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_s3_http PRIVATE _GNU_SOURCE)

# S3 object store test. Like the NVMe-oF one, the target takes its DMA
# through a callback pair, so a byte array stands in for guest memory and
# no VM, RDMA device or server process is involved.
add_executable(test_s3_target
    test_s3_target.c
    ${PROJECT_SOURCE_DIR}/src/s3_target.c
    ${PROJECT_SOURCE_DIR}/src/s3_http.c
    ${PROJECT_SOURCE_DIR}/src/s3_token.c
)
target_include_directories(test_s3_target PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_s3_target PRIVATE _GNU_SOURCE)

# In-band TCP endpoint test. It drives the endpoint with hand-built
# Ethernet frames and dissects what comes back, so the ARP exchange, the
# handshake, segment reassembly and the checksums are all checked against
# an independent implementation in the test.
add_executable(test_s3_tcp
    test_s3_tcp.c
    ${PROJECT_SOURCE_DIR}/src/s3_tcp.c
    ${PROJECT_SOURCE_DIR}/src/s3_target.c
    ${PROJECT_SOURCE_DIR}/src/s3_http.c
    ${PROJECT_SOURCE_DIR}/src/s3_token.c
)
target_include_directories(test_s3_tcp PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(test_s3_tcp PRIVATE _GNU_SOURCE)

# ionic MSI-X mask/pending test. It links the ionic emulator, reaches the
# per-vector interrupt state through ionic_eth_emu_priv.h, and stubs
# vfu_irq_trigger() with a per-vector counter, so no vfio-user link or live
# context is needed.
add_executable(test_ionic_intr_pending
    test_ionic_intr_pending.c
    ${PROJECT_SOURCE_DIR}/src/ionic_eth_emu.c
)
target_include_directories(test_ionic_intr_pending PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
# Third-party headers are SYSTEM includes, matching the main target, so
# their warnings stay quiet while the ionic sources under test are still
# warning-checked.
target_include_directories(test_ionic_intr_pending SYSTEM PRIVATE
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_ionic_intr_pending PRIVATE
    _GNU_SOURCE)

# ionic PORT_INIT port-info test. It links the eth emulator, calls its
# handlers through ionic_eth_emu_priv.h, and backs the DMA entry points with
# a plain buffer, so the structure the driver reads its link speed out of
# can be inspected directly -- no guest, no VM and no vfio-user socket.
add_executable(test_ionic_port_info
    test_ionic_port_info.c
    ${PROJECT_SOURCE_DIR}/src/ionic_eth_emu.c
)
target_include_directories(test_ionic_port_info PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
# Third-party headers are SYSTEM includes, matching the main target, so
# their warnings stay quiet while the ionic sources under test are still
# warning-checked.
target_include_directories(test_ionic_port_info SYSTEM PRIVATE
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_ionic_port_info PRIVATE
    _GNU_SOURCE)

# Server lifecycle test: forks the real server and drives it through
# startup, clients that drop out of vfio-user negotiation, and SIGTERM/SIGINT
# shutdown. Needs no symbols from the server, so it is a plain program. The
# server's path is compiled in rather than passed on the command line: a
# program that runs whatever its arguments name is what CodeQL's
# cpp/uncontrolled-process-operation reports.
add_executable(test_server_lifecycle test_server_lifecycle.c)
target_compile_definitions(test_server_lifecycle PRIVATE _GNU_SOURCE
    "ERNIC_SERVER_PATH=\"$<TARGET_FILE:rocm-ernic>\"")
add_dependencies(test_server_lifecycle rocm-ernic)

# ionic QUERY_QP test. It links the admin queue, calls
# ionic_adminq_dispatch_wqe() through ionic_adminq_priv.h, and backs the DMA
# entry points with a plain buffer, so the side buffers the driver reads its
# answer out of can be inspected directly.
add_executable(test_ionic_query_qp
    test_ionic_query_qp.c
    ${PROJECT_SOURCE_DIR}/src/ionic_adminq.c
)
target_include_directories(test_ionic_query_qp PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_include_directories(test_ionic_query_qp SYSTEM PRIVATE
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_ionic_query_qp PRIVATE
    _GNU_SOURCE)

# ionic SRQ adminq unit test: drives CREATE/MODIFY/QUERY/DESTROY_SRQ through
# ionic_adminq_dispatch_wqe() against the same fake guest memory, so the
# response buffer AND the reported length can both be inspected -- the pair
# that was wrong while CREATE_SRQ wrote an id the driver was never told to
# read. Also pins the id-map capacity against the advertised max_srq.
add_executable(test_ionic_srq_adminq
    test_ionic_srq_adminq.c
    ${PROJECT_SOURCE_DIR}/src/ionic_adminq.c
)
target_include_directories(test_ionic_srq_adminq PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_include_directories(test_ionic_srq_adminq SYSTEM PRIVATE
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_ionic_srq_adminq PRIVATE
    _GNU_SOURCE)

# ionic CQ arm-semantics test. It links the datapath, reaches the private CQ
# state through ionic_datapath_priv.h, and counts the event callback
# in-process, so no guest memory is needed.
add_executable(test_ionic_cq_arm
    test_ionic_cq_arm.c
    ${PROJECT_SOURCE_DIR}/src/ionic_datapath.c
)
target_include_directories(test_ionic_cq_arm PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
# Third-party headers are SYSTEM includes, matching the main target, so
# their warnings stay quiet while the ionic sources under test are still
# warning-checked.
target_include_directories(test_ionic_cq_arm SYSTEM PRIVATE
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_ionic_cq_arm PRIVATE _GNU_SOURCE)
# libvfio-user supplies vfu_log/vfu_sgl_*; the arm path never reaches them, so
# the real library is cheaper than stubbing its ABI. Everything the datapath
# needs from the rest of this project is stubbed in the test itself.
target_link_directories(test_ionic_cq_arm PRIVATE
    ${VFIO_USER_LIBRARY_DIRS} ${GLIB2_LIBRARY_DIRS})
target_link_libraries(test_ionic_cq_arm PRIVATE
    ${VFIO_USER_LINK_LIBRARIES} ${GLIB2_LIBRARIES} pthread)

# ionic receive-ring and buffer test. Like the CQ arm test, it links the
# datapath and reaches its rings through ionic_datapath_priv.h.
add_executable(test_ionic_rq_wrap
    test_ionic_rq_wrap.c
    ${PROJECT_SOURCE_DIR}/src/ionic_datapath.c
)
target_include_directories(test_ionic_rq_wrap PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
# Third-party headers are SYSTEM includes, matching the main target, so
# their warnings stay quiet while the ionic sources under test are still
# warning-checked.
target_include_directories(test_ionic_rq_wrap SYSTEM PRIVATE
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_ionic_rq_wrap PRIVATE _GNU_SOURCE)
# libvfio-user supplies vfu_log/vfu_sgl_*; the arm path never reaches them, so
# the real library is cheaper than stubbing its ABI. Everything the datapath
# needs from the rest of this project is stubbed in the test itself.
target_link_directories(test_ionic_rq_wrap PRIVATE
    ${VFIO_USER_LIBRARY_DIRS} ${GLIB2_LIBRARY_DIRS})
target_link_libraries(test_ionic_rq_wrap PRIVATE
    ${VFIO_USER_LINK_LIBRARIES} ${GLIB2_LIBRARIES} pthread)

# TCP mesh-topology broadcast test. It links the TCP backend, reaches its
# helpers through rdma_backend_tcp_priv.h, and stubs the backend's externals,
# so no RDMA subsystem link is needed; the broadcast goes over a socketpair.
add_executable(test_tcp_mesh_topology
    test_tcp_mesh_topology.c
    ${PROJECT_SOURCE_DIR}/src/rdma/rdma_backend_tcp.c
)
target_include_directories(test_tcp_mesh_topology PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/qemu-compat/include
    ${PROJECT_SOURCE_DIR}/shared
)
# Vendored and third-party header trees are SYSTEM includes, matching the
# main target, so the backend and the test are still warning-checked.
target_include_directories(test_tcp_mesh_topology SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third-party/qemu/include
    ${PROJECT_SOURCE_DIR}/third-party/qemu/hw/rdma
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_tcp_mesh_topology PRIVATE
    _GNU_SOURCE)
# ASan's use-after-return detection would move the broadcast's frame off
# the poisoned stack the test gives it
ernic_disable_asan_use_after_return(test_tcp_mesh_topology)
target_link_directories(test_tcp_mesh_topology PRIVATE
    ${GLIB2_LIBRARY_DIRS} ${IBVERBS_LIBRARY_DIRS})
target_link_libraries(test_tcp_mesh_topology PRIVATE
    ${GLIB2_LIBRARIES} ${IBVERBS_LIBRARIES} pthread)

# TCP GID -> node resolution test. Like the mesh-topology test it links the
# TCP backend, reaches its helpers through rdma_backend_tcp_priv.h, and stubs
# the backend's externals; resolution needs only the GID table, so no socket
# or thread is involved.
add_executable(test_tcp_gid_resolve
    test_tcp_gid_resolve.c
    ${PROJECT_SOURCE_DIR}/src/rdma/rdma_backend_tcp.c
)
target_include_directories(test_tcp_gid_resolve PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/qemu-compat/include
    ${PROJECT_SOURCE_DIR}/shared
)
# Vendored and third-party header trees are SYSTEM includes, matching the
# main target, so the backend and the test are still warning-checked.
target_include_directories(test_tcp_gid_resolve SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third-party/qemu/include
    ${PROJECT_SOURCE_DIR}/third-party/qemu/hw/rdma
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_tcp_gid_resolve PRIVATE
    _GNU_SOURCE)
target_link_directories(test_tcp_gid_resolve PRIVATE
    ${GLIB2_LIBRARY_DIRS} ${IBVERBS_LIBRARY_DIRS})
target_link_libraries(test_tcp_gid_resolve PRIVATE
    ${GLIB2_LIBRARIES} ${IBVERBS_LIBRARIES} pthread)

# TCP thread stop test. It links the TCP backend and runs the real accept,
# health-check and receive threads on loopback sockets and socketpairs, with
# the thread entry points backed by pthreads, so a stop that never returns
# fails the test.
add_executable(ernic-test-tcp-thread-stop
    test_tcp_thread_stop.c
    ${PROJECT_SOURCE_DIR}/src/rdma/rdma_backend_tcp.c
)
target_include_directories(ernic-test-tcp-thread-stop PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/qemu-compat/include
    ${PROJECT_SOURCE_DIR}/shared
)
# Vendored and third-party header trees are SYSTEM includes, matching the
# main target, so the backend and the test are still warning-checked.
target_include_directories(ernic-test-tcp-thread-stop SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third-party/qemu/include
    ${PROJECT_SOURCE_DIR}/third-party/qemu/hw/rdma
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(ernic-test-tcp-thread-stop PRIVATE
    _GNU_SOURCE)
# The test skips a row that can't work under valgrind, which it detects
# with RUNNING_ON_VALGRIND from valgrind's header. Without the header, the
# row always runs.
include(CheckIncludeFile)
check_include_file(valgrind/valgrind.h ERNIC_HAVE_VALGRIND_H)
if(ERNIC_HAVE_VALGRIND_H)
    target_compile_definitions(ernic-test-tcp-thread-stop PRIVATE
        ERNIC_HAVE_VALGRIND_H)
endif()
target_link_directories(ernic-test-tcp-thread-stop PRIVATE
    ${GLIB2_LIBRARY_DIRS} ${IBVERBS_LIBRARY_DIRS})
target_link_libraries(ernic-test-tcp-thread-stop PRIVATE
    ${GLIB2_LIBRARIES} ${IBVERBS_LIBRARIES} pthread)

# RDMA resource-table allocator test. rdma_rm.c keeps its allocator
# static inline, so the test drives it through the resource
# manager's CQ table. It stubs the backend entry points rdma_rm.c
# calls, so no RDMA subsystem link is needed. ERNIC_TEST_VENDORED_SOURCES
# silences rdma_rm.c with -w, and the test's own code is still
# warning-checked.
add_executable(test_rdma_res_tbl test_rdma_res_tbl.c
    ${PROJECT_SOURCE_DIR}/third-party/qemu/hw/rdma/rdma_rm.c
    ${PROJECT_SOURCE_DIR}/src/qemu-compat/error-report.c
    ${PROJECT_SOURCE_DIR}/src/qemu-compat/qemu_stubs.c
)
target_include_directories(test_rdma_res_tbl PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/rdma
    ${PROJECT_SOURCE_DIR}/src/qemu-compat/include
)
# Vendored and third-party header trees are SYSTEM includes, matching the
# main target.
target_include_directories(test_rdma_res_tbl SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third-party/qemu/include
    ${PROJECT_SOURCE_DIR}/third-party/qemu/hw/rdma
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_rdma_res_tbl PRIVATE
    _GNU_SOURCE)
target_link_directories(test_rdma_res_tbl PRIVATE
    ${GLIB2_LIBRARY_DIRS} ${IBVERBS_LIBRARY_DIRS})
target_link_libraries(test_rdma_res_tbl PRIVATE
    ${GLIB2_LIBRARIES} ${IBVERBS_LIBRARIES} pthread)

# QEMU utility stubs test: bitmap word boundaries and the oversize-map
# abort. It links the stubs translation unit directly. The vendored header trees are SYSTEM includes, as in the main
# build, so the test source itself is still warning-checked.
add_executable(test_qemu_stubs test_qemu_stubs.c
    ${PROJECT_SOURCE_DIR}/src/qemu-compat/qemu_stubs.c
)
target_include_directories(test_qemu_stubs PRIVATE
    ${PROJECT_SOURCE_DIR}/src/qemu-compat/include
)
target_include_directories(test_qemu_stubs SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third-party/qemu/hw/rdma
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_qemu_stubs PRIVATE
    _GNU_SOURCE)
target_link_directories(test_qemu_stubs PRIVATE
    ${GLIB2_LIBRARY_DIRS} ${IBVERBS_LIBRARY_DIRS})
target_link_libraries(test_qemu_stubs PRIVATE
    ${GLIB2_LIBRARIES} ${IBVERBS_LIBRARIES} pthread)

# In-process ionic device harness. It links every source the server is
# built from except rocm_ernic_server.c, with fake_vfu.c standing in for
# libvfio-user, and ionic_harness.c drives the device the way the guest
# driver does: registers, doorbells, and rings in fake guest memory. No VM,
# socket or RDMA device is involved, so it runs in every build leg. The
# source lists come from the top-level CMakeLists.txt, so the harness keeps
# linking what the server links.
set(ERNIC_HARNESS_DEVICE_SOURCES
    ${BRIDGE_SOURCES}
    ${IONIC_EMU_SOURCES}
    ${NVMEOF_SOURCES}
    ${S3_SOURCES}
    ${RDMA_BACKEND_SOURCES}
    ${QEMU_COMPAT_SOURCES}
)
list(TRANSFORM ERNIC_HARNESS_DEVICE_SOURCES PREPEND "${PROJECT_SOURCE_DIR}/")
set(ERNIC_HARNESS_VENDORED_SOURCES ${ERNIC_VENDORED_SOURCES})
list(TRANSFORM ERNIC_HARNESS_VENDORED_SOURCES
    PREPEND "${PROJECT_SOURCE_DIR}/")
set_source_files_properties(${ERNIC_HARNESS_VENDORED_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-w"
)
add_executable(ernic-test-ionic-harness
    test_ionic_harness.c
    ionic_harness.c
    fake_vfu.c
    ${ERNIC_HARNESS_DEVICE_SOURCES}
    ${ERNIC_HARNESS_VENDORED_SOURCES}
)
target_include_directories(ernic-test-ionic-harness PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/rdma
    ${PROJECT_SOURCE_DIR}/src/qemu-compat/include
    ${PROJECT_SOURCE_DIR}/shared
)
# Vendored and third-party header trees are SYSTEM includes, matching the
# main target.
target_include_directories(ernic-test-ionic-harness SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third-party/qemu/include
    ${PROJECT_SOURCE_DIR}/third-party/qemu/hw/rdma
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(ernic-test-ionic-harness PRIVATE _GNU_SOURCE)
target_compile_options(ernic-test-ionic-harness PRIVATE
    ${GLIB2_CFLAGS_OTHER})
# No libvfio-user: fake_vfu.c defines the entry points the device calls.
target_link_directories(ernic-test-ionic-harness PRIVATE
    ${GLIB2_LIBRARY_DIRS} ${IBVERBS_LIBRARY_DIRS})
target_link_libraries(ernic-test-ionic-harness PRIVATE
    ${GLIB2_LIBRARIES} ${IBVERBS_LIBRARIES} pthread)

# ---------------------------------------------------------------
# Apply the project warning / hardening flags to every test target
# ---------------------------------------------------------------
set(ERNIC_TEST_TARGETS
    test_pci_client
    test_data_transfer
    test_rdma_cm
    test_srq
    test_srq_transfer
    test_ud_transfer
    test_ud_sq_reclaim
    test_ud_mesh
    test_ernic_device
    test_write_imm
    test_parse_int
    test_nvmeof_target
    test_nvmeof_cm
    test_s3_token
    test_s3_http
    test_s3_target
    test_s3_tcp
    test_ionic_intr_pending
    test_ionic_port_info
    test_ionic_query_qp
    test_server_lifecycle
    test_ionic_srq_adminq
    test_ionic_cq_arm
    test_ionic_rq_wrap
    test_tcp_mesh_topology
    test_tcp_gid_resolve
    ernic-test-tcp-thread-stop
    test_rdma_res_tbl
    test_qemu_stubs
    ernic-test-ionic-harness
)

foreach(ernic_test_target IN LISTS ERNIC_TEST_TARGETS)
    ernic_set_compiler_flags(${ernic_test_target})
endforeach()

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

add_test(
    NAME ernic-device-tuple-unit
    COMMAND $<TARGET_FILE:test_ernic_device>
)
set_tests_properties(ernic-device-tuple-unit PROPERTIES
    TIMEOUT 30
    RUN_SERIAL TRUE
)

# test_data_transfer, test_write_imm and test_rdma_cm -- and the guest-only
# SRQ and UD tests built alongside them -- are deliberately not
# registered with CTest. They need a real RDMA device, which no host running
# ctest has -- hosted runners and the self-hosted pool both run ctest outside
# the guest. Registered here they returned 77 and CTest reported the skip as
# a pass, so they looked like coverage while testing nothing. They are built
# (test_data_transfer is the loopback config step's TEST_BIN) and are run for
# real inside the VM by the loopback job in system-tests.yml, where a device
# is guaranteed and exit 77 is treated as a failure.

# Strict integer parser unit test (table-driven)
add_test(
    NAME parse-int-unit
    COMMAND $<TARGET_FILE:test_parse_int>
)
set_tests_properties(parse-int-unit PROPERTIES TIMEOUT 30)

# NVMe-oF controller unit test: drives the capsule sequence a real
# nvme-rdma initiator emits, from Fabrics Connect to read-after-write.
add_test(
    NAME nvmeof-target-unit
    COMMAND $<TARGET_FILE:test_nvmeof_target>
)
set_tests_properties(nvmeof-target-unit PROPERTIES TIMEOUT 30)

# IB CM responder unit test: REQ/REP/RTU/DREQ, plus every rejection path
# a misaddressed or mismatched "nvme connect" can take.
add_test(
    NAME nvmeof-cm-unit
    COMMAND $<TARGET_FILE:test_nvmeof_cm>
)
set_tests_properties(nvmeof-cm-unit PROPERTIES TIMEOUT 30)

# S3-over-RDMA token codec unit test: the x-amz-rdma-token and
# x-amz-rdma-reply wire formats, pinned against a hand-assembled image.
add_test(
    NAME s3-token-unit
    COMMAND $<TARGET_FILE:test_s3_token>
)
set_tests_properties(s3-token-unit PROPERTIES TIMEOUT 30)

# S3 control-plane HTTP unit test: request parsing including incremental
# arrival, query handling, and the response builder.
add_test(
    NAME s3-http-unit
    COMMAND $<TARGET_FILE:test_s3_http>
)
set_tests_properties(s3-http-unit PROPERTIES TIMEOUT 30)

# S3 object store unit test: PUT, GET, ranged GET, HEAD, DELETE,
# ListObjectsV2 and the multipart upload sequence, over a fake DMA.
add_test(
    NAME s3-target-unit
    COMMAND $<TARGET_FILE:test_s3_target>
)
set_tests_properties(s3-target-unit PROPERTIES TIMEOUT 60)

# In-band TCP endpoint unit test: ARP, the handshake, split segments,
# keep-alive and teardown, driven with raw Ethernet frames.
add_test(
    NAME s3-tcp-unit
    COMMAND $<TARGET_FILE:test_s3_tcp>
)
set_tests_properties(s3-tcp-unit PROPERTIES TIMEOUT 60)

# ionic MSI-X mask/pending unit test: asserts an assertion raised on a masked
# vector is latched and replayed on unmask rather than dropped.
add_test(
    NAME ionic-intr-pending-unit
    COMMAND $<TARGET_FILE:test_ionic_intr_pending>
)
set_tests_properties(ionic-intr-pending-unit PROPERTIES TIMEOUT 30)

# ionic PORT_INIT unit test: asserts the port-info block the driver reads is
# actually published. While PORT_INIT was a stub it stayed zeroed, the
# transceiver read back as absent, ionic_get_link_ksettings() resolved
# PORT_NONE and never assigned a speed -- so ethtool reported "Unknown!" and
# UCX refused to build an endpoint on a device with bandwidth 0.
add_test(
    NAME ionic-port-info-unit
    COMMAND $<TARGET_FILE:test_ionic_port_info>
)
set_tests_properties(ionic-port-info-unit PROPERTIES TIMEOUT 30)

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
    SKIP_RETURN_CODE 77
)

# ionic QUERY_QP unit test: asserts the command writes the QP's real state
# into the side buffers the driver reads, rather than leaving them zeroed and
# reporting every queue pair as RESET.
add_test(
    NAME ionic-query-qp-unit
    COMMAND $<TARGET_FILE:test_ionic_query_qp>
)
set_tests_properties(ionic-query-qp-unit PROPERTIES TIMEOUT 30)

# ionic SRQ adminq unit test: the four SRQ handlers had no in-process
# coverage, which is how CREATE_SRQ shipped writing its id into a response
# the driver was told was zero bytes long.
add_test(
    NAME ionic-srq-adminq-unit
    COMMAND $<TARGET_FILE:test_ionic_srq_adminq>
)
set_tests_properties(ionic-srq-adminq-unit PROPERTIES TIMEOUT 30)

# ionic CQ arm unit test: asserts arming a CQ that still holds unreaped CQEs
# raises the event immediately, rather than waiting for a CQE that never comes.
add_test(
    NAME ionic-cq-arm-unit
    COMMAND $<TARGET_FILE:test_ionic_cq_arm>
)
set_tests_properties(ionic-cq-arm-unit PROPERTIES TIMEOUT 30)

# ionic receive-ring unit test: the doorbell's producer index is masked to the
# ring by the driver, so reading it as a free-running counter broke on the
# first wrap and reported tens of thousands of receives outstanding.
add_test(
    NAME ionic-rq-wrap-unit
    COMMAND $<TARGET_FILE:test_ionic_rq_wrap>
)
set_tests_properties(ionic-rq-wrap-unit PROPERTIES TIMEOUT 30)

# TCP mesh-topology broadcast unit test: asserts the unused node slots in the
# fixed-size payload are zeroed rather than carrying stack contents.
add_test(
    NAME tcp-mesh-topology-unit
    COMMAND $<TARGET_FILE:test_tcp_mesh_topology>
)
set_tests_properties(tcp-mesh-topology-unit PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

# TCP GID -> node resolution unit test: exact 16-byte matching, no-match
# failure, the zero-filled advertisement tail, and the mesh entry points
# leaving a non-TCP backend's private block alone.
add_test(
    NAME tcp-gid-resolve-unit
    COMMAND $<TARGET_FILE:test_tcp_gid_resolve>
)
set_tests_properties(tcp-gid-resolve-unit PROPERTIES TIMEOUT 30)

# TCP thread stop unit test: asserts stopping the accept thread returns while
# it is blocked in accept(), stopping the health-check thread returns without
# waiting out its interval, every receive thread is stopped and joined once,
# and tcp_fini() returns while a manager thread is blocked sending to a worker
# that stopped reading, which is what lets tcp_fini() finish on SIGTERM
# promptly and free the backend with no thread still using it.
add_test(
    NAME tcp-thread-stop-unit
    COMMAND $<TARGET_FILE:ernic-test-tcp-thread-stop>
)
# The test spends most of its time waiting on purpose: about 40 s, and over
# 50 s under valgrind in CI. Each row's alarm() still turns a hang into a
# named failure long before this.
set_tests_properties(tcp-thread-stop-unit PROPERTIES TIMEOUT 120)

# RDMA resource-table allocator unit test: asserts a full table refuses the
# next allocation rather than handing back a handle one past the end.
add_test(
    NAME rdma-res-tbl-unit
    COMMAND $<TARGET_FILE:test_rdma_res_tbl>
)
set_tests_properties(rdma-res-tbl-unit PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

# QEMU utility stubs unit test (bitmap)
add_test(
    NAME qemu-stubs-unit
    COMMAND $<TARGET_FILE:test_qemu_stubs>
)
set_tests_properties(qemu-stubs-unit PROPERTIES TIMEOUT 30)

# In-process ionic device harness: the whole device, driven as the guest
# driver drives it, one CTest test per group so a failure names its area.
# Bring-up and devcmds, refused admin commands, RC transfers, atomics and
# fast registration, CQ events, UD, SRQ, Ethernet Tx/Rx, and the NVMe-oF
# CM handshake and Connect over the GSI QP.
foreach(ernic_harness_group
        devcmds bringup admin rc atomic cq-events ud srq eth nvmeof teardown)
    add_test(
        NAME ionic-harness-${ernic_harness_group}
        COMMAND $<TARGET_FILE:ernic-test-ionic-harness>
            ${ernic_harness_group}
    )
    set_tests_properties(ionic-harness-${ernic_harness_group} PROPERTIES
        TIMEOUT 60)
endforeach()

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
    SKIP_RETURN_CODE 77
)

# ionic CI test suite: comprehensive checks on the ionic emulation path
# (VID:DID, BAR layout, MSI-X vectors, multi-backend startup, clean shutdown).
# Does not require a VM or RDMA hardware.
add_test(
    NAME ionic-ci
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_ionic_ci.sh
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
)
set_tests_properties(ionic-ci PROPERTIES
    TIMEOUT 60
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
    ENVIRONMENT "SERVER_BIN=$<TARGET_FILE:rocm-ernic>"
)

# TCP mesh registration suite: GID advertisement across a three-node mesh,
# an unconfigured node, a contested GID, and the protocol version gate in
# both directions. Drives real servers over loopback TCP; no VM or RDMA
# hardware needed. RUN_SERIAL because it starts several servers at once.
add_test(
    NAME tcp-mesh-ci
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_tcp_mesh_ci.sh
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
)
# Generous timeout: the suite starts several servers and waits on mesh
# convergence, and runs in every build leg including thread-sanitizer
# Release, where that chain is slow. The script's own budgets are the real
# bound; this only has to be larger than them.
set_tests_properties(tcp-mesh-ci PROPERTIES
    TIMEOUT 420
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
    ENVIRONMENT "SERVER_BIN=$<TARGET_FILE:rocm-ernic>"
)

# NVMe-oF backend CI suite: every documented option spelling starts a server
# with the namespace it asked for, a file-backed namespace appears on disk at
# the right size, and bad options are refused before startup.
# Does not require a VM or RDMA hardware.
add_test(
    NAME nvmeof-ci
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_nvmeof_ci.sh
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
)
set_tests_properties(nvmeof-ci PROPERTIES
    TIMEOUT 120
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
    ENVIRONMENT "SERVER_BIN=$<TARGET_FILE:rocm-ernic>"
)

# S3 backend CI suite: every documented option spelling starts a server with
# the store it asked for, the in-band endpoint announces the address and MAC
# the guest will ARP for, and bad options are refused before startup.
# Does not require a VM or RDMA hardware.
add_test(
    NAME s3-ci
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_s3_ci.sh
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
)
set_tests_properties(s3-ci PROPERTIES
    TIMEOUT 120
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
    ENVIRONMENT "SERVER_BIN=$<TARGET_FILE:rocm-ernic>"
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
