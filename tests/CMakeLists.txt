# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# Tests build configuration

find_package(PkgConfig REQUIRED)
pkg_check_modules(CMOCKA cmocka)

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

# -- Unit tests --------------------------------------------------
# Built with AddressSanitizer + LeakSanitizer + UBSan (unless the project
# is already built with ThreadSanitizer, which is incompatible) so a NULL
# deref, leak, or out-of-bounds read fails the test even when the rest of
# the project is built without sanitizers. Flag set matches
# cmake/ErnicSanitizers.cmake; -fno-omit-frame-pointer is compile-only.
set(ERNIC_UNIT_TEST_SAN)
set(ERNIC_UNIT_TEST_SAN_LINK)
if(NOT ERNIC_USE_THREAD_SANITIZER)
    set(ERNIC_UNIT_TEST_SAN
        -fsanitize=address,undefined,leak -fno-omit-frame-pointer)
    set(ERNIC_UNIT_TEST_SAN_LINK -fsanitize=address,undefined,leak)
endif()

# QEMU-ported sources that tests compile as separate translation units.
# Suppressing warnings per-source rather than per-target keeps the ported
# C quiet (matching the main build) while the test's own source is still
# warning-checked -- a target-wide -w would disable warnings everywhere,
# since GCC honours -w regardless of where it appears in the flag list.
set(ERNIC_TEST_PORTED_SOURCES
    ${CMAKE_SOURCE_DIR}/src/from-qemu/hw/rdma/rdma_backend.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/hw/rdma/rdma_utils.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/hw/rdma/rdma_rm.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/rdma_cm_proto.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/dhcp_server.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/error-report.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/qemu-stubs/qemu_stubs.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/qemu-stubs/qemu_type_stubs.c
)
set_source_files_properties(${ERNIC_TEST_PORTED_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-w"
)

# rdma_backend_query_port test. Links the backend TU plus the utility /
# resource-manager / qemu-stub sources it depends on; the query_port paths
# under test never reach libibverbs or the DMA layer.
add_executable(test_rdma_backend_query_port
    test_rdma_backend_query_port.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/hw/rdma/rdma_backend.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/hw/rdma/rdma_utils.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/hw/rdma/rdma_rm.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/error-report.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/qemu-stubs/qemu_stubs.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/qemu-stubs/qemu_type_stubs.c
)
target_include_directories(test_rdma_backend_query_port PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
# The ported header trees are SYSTEM includes so their warnings stay quiet
# while the test source itself is still warning-checked. The ported .c files
# are silenced per-source via ERNIC_TEST_PORTED_SOURCES.
target_include_directories(test_rdma_backend_query_port SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/src/from-qemu
    ${CMAKE_SOURCE_DIR}/src/from-qemu/hw/rdma
    ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_rdma_backend_query_port PRIVATE
    _GNU_SOURCE)
target_compile_options(test_rdma_backend_query_port PRIVATE
    ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_rdma_backend_query_port PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})
target_link_directories(test_rdma_backend_query_port PRIVATE
    ${GLIB2_LIBRARY_DIRS} ${IBVERBS_LIBRARY_DIRS})
target_link_libraries(test_rdma_backend_query_port PRIVATE
    ${GLIB2_LIBRARIES} ${IBVERBS_LIBRARIES} pthread)

# -- test_write_imm (requires libibverbs) ------------------------
add_executable(test_write_imm
    test_write_imm.c
)
target_compile_definitions(test_write_imm PRIVATE
    _POSIX_C_SOURCE=200809L
)
target_include_directories(test_write_imm PRIVATE
    ${IBVERBS_INCLUDE_DIRS}
)
target_link_directories(test_write_imm PRIVATE
    ${IBVERBS_LIBRARY_DIRS}
)
target_link_libraries(test_write_imm PRIVATE
    ${IBVERBS_LIBRARIES}
)

# net_headers.h checksum helpers are header-only and self-contained. The
# ported header is a SYSTEM include so its warnings stay quiet while the
# test source itself is still warning-checked.
add_executable(test_net_checksum test_net_checksum.c)
target_include_directories(test_net_checksum SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils
)
target_compile_options(test_net_checksum PRIVATE ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_net_checksum PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})

# -- Parser unit tests -------------------------------------------
# These link the QEMU-ported wire parsers directly and are built with
# AddressSanitizer + UBSan (unless the project is already built with
# ThreadSanitizer, which is incompatible), so an out-of-bounds read fails
# the test even when the rest of the project is built without sanitizers.
set(ERNIC_PARSER_TEST_SAN "")
if(NOT ERNIC_USE_THREAD_SANITIZER)
    set(ERNIC_PARSER_TEST_SAN
        -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

add_executable(test_rdma_cm_proto
    test_rdma_cm_proto.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/rdma_cm_proto.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/error-report.c
)
target_include_directories(test_rdma_cm_proto PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
# Ported header trees are SYSTEM includes; the ported .c files are silenced
# per-source via ERNIC_TEST_PORTED_SOURCES.
target_include_directories(test_rdma_cm_proto SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/src/from-qemu
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils
    ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
    ${GLIB2_INCLUDE_DIRS}
)
target_compile_definitions(test_rdma_cm_proto PRIVATE _GNU_SOURCE)
target_compile_options(test_rdma_cm_proto PRIVATE
    ${ERNIC_PARSER_TEST_SAN})
target_link_options(test_rdma_cm_proto PRIVATE ${ERNIC_PARSER_TEST_SAN})
target_link_directories(test_rdma_cm_proto PRIVATE ${GLIB2_LIBRARY_DIRS})
target_link_libraries(test_rdma_cm_proto PRIVATE ${GLIB2_LIBRARIES})

add_executable(test_dhcp_server_process
    test_dhcp_server_process.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/dhcp_server.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/error-report.c
)
target_include_directories(test_dhcp_server_process PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
# Ported header trees are SYSTEM includes; the ported .c files are silenced
# per-source via ERNIC_TEST_PORTED_SOURCES.
target_include_directories(test_dhcp_server_process SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/src/from-qemu
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils
    ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
    ${GLIB2_INCLUDE_DIRS}
)
target_compile_definitions(test_dhcp_server_process PRIVATE _GNU_SOURCE)
target_compile_options(test_dhcp_server_process PRIVATE
    ${ERNIC_PARSER_TEST_SAN})
target_link_options(test_dhcp_server_process PRIVATE ${ERNIC_PARSER_TEST_SAN})
target_link_directories(test_dhcp_server_process PRIVATE ${GLIB2_LIBRARY_DIRS})
target_link_libraries(test_dhcp_server_process PRIVATE ${GLIB2_LIBRARIES})

# -- DHCP server lifecycle / leak test ---------------------------
# Reuses the shared ERNIC_UNIT_TEST_SAN flags (ASan + LeakSanitizer + UBSan)
# so a leak in the DHCP server's create/destroy path fails the test.
add_executable(test_dhcp_server_lifecycle
    test_dhcp_server_lifecycle.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/dhcp_server.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/error-report.c
)
target_include_directories(test_dhcp_server_lifecycle PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
# Ported header trees are SYSTEM includes; the ported .c files are silenced
# per-source via ERNIC_TEST_PORTED_SOURCES.
target_include_directories(test_dhcp_server_lifecycle SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/src/from-qemu
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils
    ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
    ${GLIB2_INCLUDE_DIRS}
)
target_compile_definitions(test_dhcp_server_lifecycle PRIVATE _GNU_SOURCE)
target_compile_options(test_dhcp_server_lifecycle PRIVATE
    ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_dhcp_server_lifecycle PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})
target_link_directories(test_dhcp_server_lifecycle PRIVATE
    ${GLIB2_LIBRARY_DIRS})
target_link_libraries(test_dhcp_server_lifecycle PRIVATE ${GLIB2_LIBRARIES})

# Strict integer parser (parse_int.h) unit test (header-only). The ported
# header dir is a SYSTEM include so its warnings stay quiet while the test
# source itself is still warning-checked.
add_executable(test_parse_int test_parse_int.c)
target_include_directories(test_parse_int SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils
)
target_compile_options(test_parse_int PRIVATE ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_parse_int PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})

# NVMe-oF controller test. The controller core takes its DMA through a
# callback pair, so the test links the real translation unit and stands a
# byte array in for guest memory -- no VM, no RDMA device, no server.
add_executable(test_nvmeof_target
    test_nvmeof_target.c
    ${CMAKE_SOURCE_DIR}/src/nvmeof_target.c
)
target_include_directories(test_nvmeof_target PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_include_directories(test_nvmeof_target SYSTEM PRIVATE
    ${VFIO_USER_INCLUDE_DIRS}
)
target_compile_definitions(test_nvmeof_target PRIVATE _GNU_SOURCE)
target_compile_options(test_nvmeof_target PRIVATE ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_nvmeof_target PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})

# IB CM responder test. Feeds it the CM MADs a guest running rdma_cm
# emits and dissects the replies; no MAD ever reaches a wire.
add_executable(test_nvmeof_cm
    test_nvmeof_cm.c
    ${CMAKE_SOURCE_DIR}/src/nvmeof_cm.c
    ${CMAKE_SOURCE_DIR}/src/nvmeof_target.c
)
target_include_directories(test_nvmeof_cm PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_include_directories(test_nvmeof_cm SYSTEM PRIVATE
    ${VFIO_USER_INCLUDE_DIRS}
)
target_compile_definitions(test_nvmeof_cm PRIVATE _GNU_SOURCE)
target_compile_options(test_nvmeof_cm PRIVATE ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_nvmeof_cm PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})

# S3-over-RDMA token codec test. The encoding is a wire contract shared
# with hipObject, so this links nothing but the codec itself.
add_executable(test_s3_token
    test_s3_token.c
    ${CMAKE_SOURCE_DIR}/src/s3_token.c
)
target_include_directories(test_s3_token PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_s3_token PRIVATE _GNU_SOURCE)
target_compile_options(test_s3_token PRIVATE ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_s3_token PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})

# S3 control-plane HTTP subset test: buffers in, buffers out, no socket.
add_executable(test_s3_http
    test_s3_http.c
    ${CMAKE_SOURCE_DIR}/src/s3_http.c
)
target_include_directories(test_s3_http PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_s3_http PRIVATE _GNU_SOURCE)
target_compile_options(test_s3_http PRIVATE ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_s3_http PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})

# S3 object store test. Like the NVMe-oF one, the target takes its DMA
# through a callback pair, so a byte array stands in for guest memory and
# no VM, RDMA device or server process is involved.
add_executable(test_s3_target
    test_s3_target.c
    ${CMAKE_SOURCE_DIR}/src/s3_target.c
    ${CMAKE_SOURCE_DIR}/src/s3_http.c
    ${CMAKE_SOURCE_DIR}/src/s3_token.c
)
target_include_directories(test_s3_target PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_s3_target PRIVATE _GNU_SOURCE)
target_compile_options(test_s3_target PRIVATE ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_s3_target PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})

# In-band TCP endpoint test. It drives the endpoint with hand-built
# Ethernet frames and dissects what comes back, so the ARP exchange, the
# handshake, segment reassembly and the checksums are all checked against
# an independent implementation in the test.
add_executable(test_s3_tcp
    test_s3_tcp.c
    ${CMAKE_SOURCE_DIR}/src/s3_tcp.c
    ${CMAKE_SOURCE_DIR}/src/s3_target.c
    ${CMAKE_SOURCE_DIR}/src/s3_http.c
    ${CMAKE_SOURCE_DIR}/src/s3_token.c
)
target_include_directories(test_s3_tcp PRIVATE ${CMAKE_SOURCE_DIR}/src)
# s3_tcp.c takes its Ethernet/IP/TCP structs from the ported net_headers.h,
# which is not warning-clean under this project's flags; the test declares
# its own copies, so only the source under test needs the SYSTEM path.
target_include_directories(test_s3_tcp SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils
)
target_compile_definitions(test_s3_tcp PRIVATE _GNU_SOURCE)
target_compile_options(test_s3_tcp PRIVATE ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_s3_tcp PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})

# Loopback SGE-copy test. It #includes the loopback translation unit to
# reach its static helpers and stubs the DMA layer with an identity
# mapping, so no RDMA subsystem link is needed.
add_executable(test_loopback_sge_copy test_loopback_sge_copy.c)
target_include_directories(test_loopback_sge_copy PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/from-qemu
    ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
    ${GLIB2_INCLUDE_DIRS}
    ${IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_loopback_sge_copy PRIVATE
    _GNU_SOURCE)
# -w: this test #includes the ported loopback translation unit, so warnings
# would come from that ported C rather than the test itself; suppress them
# to match how the ported sources are compiled in the main build. This is
# target-wide because the ported C is part of this test's own translation
# unit, so it cannot be silenced per-source; the test's own code is
# consequently not warning-checked.
target_compile_options(test_loopback_sge_copy PRIVATE
    -w ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_loopback_sge_copy PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})
target_link_directories(test_loopback_sge_copy PRIVATE
    ${GLIB2_LIBRARY_DIRS} ${IBVERBS_LIBRARY_DIRS})
target_link_libraries(test_loopback_sge_copy PRIVATE
    ${GLIB2_LIBRARIES} ${IBVERBS_LIBRARIES})

# ionic MSI-X mask/pending test. It #includes the ionic emulator translation
# unit to reach the per-vector interrupt state and stubs vfu_irq_trigger()
# with a per-vector counter, so no vfio-user link or live context is needed.
add_executable(test_ionic_intr_pending test_ionic_intr_pending.c)
target_include_directories(test_ionic_intr_pending PRIVATE
    ${CMAKE_SOURCE_DIR}/src
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
target_compile_options(test_ionic_intr_pending PRIVATE ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_ionic_intr_pending PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})

# ionic QUERY_QP test. It #includes the admin queue translation unit to reach
# dispatch_wqe() and backs the DMA entry points with a plain buffer, so the
# side buffers the driver reads its answer out of can be inspected directly.
add_executable(test_ionic_query_qp test_ionic_query_qp.c)
target_include_directories(test_ionic_query_qp PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_include_directories(test_ionic_query_qp SYSTEM PRIVATE
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_ionic_query_qp PRIVATE
    _GNU_SOURCE)
target_compile_options(test_ionic_query_qp PRIVATE ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_ionic_query_qp PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})

# ionic CQ arm-semantics test. It #includes the datapath translation unit to
# reach the private CQ state and counts the event callback in-process, so no
# vfio-user link or guest memory is needed.
add_executable(test_ionic_cq_arm test_ionic_cq_arm.c)
target_include_directories(test_ionic_cq_arm PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
# Ported and third-party header trees are SYSTEM includes, matching the main
# target, so their warnings stay quiet while the ionic sources under test are
# still warning-checked.
target_include_directories(test_ionic_cq_arm SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/src/from-qemu
    ${CMAKE_SOURCE_DIR}/src/from-qemu/hw/rdma
    ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_ionic_cq_arm PRIVATE _GNU_SOURCE)
target_compile_options(test_ionic_cq_arm PRIVATE ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_ionic_cq_arm PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})
# libvfio-user supplies vfu_log/vfu_sgl_*; the arm path never reaches them, so
# the real library is cheaper than stubbing its ABI. Everything the datapath
# needs from the rest of this project is stubbed in the test itself.
target_link_directories(test_ionic_cq_arm PRIVATE
    ${VFIO_USER_LIBRARY_DIRS} ${GLIB2_LIBRARY_DIRS})
target_link_libraries(test_ionic_cq_arm PRIVATE
    ${VFIO_USER_LINK_LIBRARIES} ${GLIB2_LIBRARIES} pthread)

# TCP mesh-topology broadcast test. It #includes the TCP backend translation
# unit to reach its static helpers and stubs the backend's externals, so no
# RDMA subsystem link is needed; the broadcast goes over a socketpair.
add_executable(test_tcp_mesh_topology test_tcp_mesh_topology.c)
target_include_directories(test_tcp_mesh_topology PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/from-qemu
    ${CMAKE_SOURCE_DIR}/src/from-qemu/hw/rdma
    ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_tcp_mesh_topology PRIVATE
    _GNU_SOURCE)
# -w: this test #includes the ported TCP backend translation unit, so warnings
# would come from that ported C rather than the test itself; suppress them
# to match how the ported sources are compiled in the main build. This is
# target-wide because the ported C is part of this test's own translation
# unit, so it cannot be silenced per-source; the test's own code is
# consequently not warning-checked.
target_compile_options(test_tcp_mesh_topology PRIVATE
    -w ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_tcp_mesh_topology PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})
target_link_directories(test_tcp_mesh_topology PRIVATE
    ${GLIB2_LIBRARY_DIRS} ${IBVERBS_LIBRARY_DIRS})
target_link_libraries(test_tcp_mesh_topology PRIVATE
    ${GLIB2_LIBRARIES} ${IBVERBS_LIBRARIES} pthread)

# TCP memory-region bounds test. Like the mesh-topology test it #includes the
# TCP backend translation unit to reach its static helpers and stubs the
# backend's externals; the wire-driven cases run the real receive thread over
# a socketpair.
add_executable(test_tcp_mr_bounds test_tcp_mr_bounds.c)
target_include_directories(test_tcp_mr_bounds PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/from-qemu
    ${CMAKE_SOURCE_DIR}/src/from-qemu/hw/rdma
    ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
    ${VFIO_USER_INCLUDE_DIRS}
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_tcp_mr_bounds PRIVATE
    _GNU_SOURCE)
# -w: this test #includes the ported TCP backend translation unit, so warnings
# would come from that ported C rather than the test itself; suppress them
# to match how the ported sources are compiled in the main build. This is
# target-wide because the ported C is part of this test's own translation
# unit, so it cannot be silenced per-source; the test's own code is
# consequently not warning-checked.
target_compile_options(test_tcp_mr_bounds PRIVATE
    -w ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_tcp_mr_bounds PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})
target_link_directories(test_tcp_mr_bounds PRIVATE
    ${GLIB2_LIBRARY_DIRS} ${IBVERBS_LIBRARY_DIRS})
target_link_libraries(test_tcp_mr_bounds PRIVATE
    ${GLIB2_LIBRARIES} ${IBVERBS_LIBRARIES} pthread)

# RDMA resource-table allocator test. It #includes the resource-manager
# translation unit to reach its static inline allocator and stubs the backend
# entry points it calls, so no RDMA subsystem link is needed.
add_executable(test_rdma_res_tbl test_rdma_res_tbl.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/hw/rdma/rdma_utils.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils/error-report.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/qemu-stubs/qemu_stubs.c
    ${CMAKE_SOURCE_DIR}/src/from-qemu/qemu-stubs/qemu_type_stubs.c
)
target_include_directories(test_rdma_res_tbl PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/from-qemu
    ${CMAKE_SOURCE_DIR}/src/from-qemu/hw/rdma
    ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
    ${GLIB2_INCLUDE_DIRS}
    ${ERNIC_IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_rdma_res_tbl PRIVATE
    _GNU_SOURCE)
# -w: this test #includes the ported resource-manager translation unit, so
# warnings would come from that ported C rather than the test itself; suppress
# them to match how the ported sources are compiled in the main build.
target_compile_options(test_rdma_res_tbl PRIVATE
    -w ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_rdma_res_tbl PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})
target_link_directories(test_rdma_res_tbl PRIVATE
    ${GLIB2_LIBRARY_DIRS} ${IBVERBS_LIBRARY_DIRS})
target_link_libraries(test_rdma_res_tbl PRIVATE
    ${GLIB2_LIBRARIES} ${IBVERBS_LIBRARIES} pthread)

# ---------------------------------------------------------------
# Apply the project warning / hardening flags to every test target
# ---------------------------------------------------------------
set(ERNIC_TEST_TARGETS
    test_pci_client
    test_data_transfer
    test_rdma_cm
    test_rdma_backend_query_port
    test_write_imm
    test_net_checksum
    test_rdma_cm_proto
    test_dhcp_server_process
    test_dhcp_server_lifecycle
    test_parse_int
    test_nvmeof_target
    test_nvmeof_cm
    test_s3_token
    test_s3_http
    test_s3_target
    test_s3_tcp
    test_loopback_sge_copy
    test_ionic_intr_pending
    test_ionic_query_qp
    test_ionic_cq_arm
    test_tcp_mesh_topology
    test_tcp_mr_bounds
    test_rdma_res_tbl
)

foreach(ernic_test_target IN LISTS ERNIC_TEST_TARGETS)
    ernic_set_compiler_flags(${ernic_test_target})
    # Match the main target: CI configures with ERNIC_WERROR=ON, and without
    # this a new warning in test code would scroll past instead of failing.
    if(ERNIC_WERROR)
        target_compile_options(${ernic_test_target} PRIVATE -Werror)
    endif()
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

# test_data_transfer, test_write_imm and test_rdma_cm are deliberately not
# registered with CTest. They need a real RDMA device, which no host running
# ctest has -- hosted runners and the self-hosted pool both run ctest outside
# the guest. Registered here they returned 77 and CTest reported the skip as
# a pass, so they looked like coverage while testing nothing. They are built
# (test_data_transfer is the loopback config step's TEST_BIN) and are run for
# real inside the VM by the loopback job in system-tests.yml, where a device
# is guaranteed and exit 77 is treated as a failure.

# rdma_backend_query_port NULL-guard / dispatch unit test
add_test(
    NAME rdma-backend-query-port-unit
    COMMAND $<TARGET_FILE:test_rdma_backend_query_port>
)
set_tests_properties(rdma-backend-query-port-unit PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

# TCP/UDP checksum helper unit test (table-driven, ASan-instrumented)
add_test(
    NAME net-checksum-unit
    COMMAND $<TARGET_FILE:test_net_checksum>
)
set_tests_properties(net-checksum-unit PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

# rdma_cm protocol parser unit test (table-driven, ASan-instrumented)
add_test(
    NAME rdma-cm-proto-unit
    COMMAND $<TARGET_FILE:test_rdma_cm_proto>
)
set_tests_properties(rdma-cm-proto-unit PROPERTIES TIMEOUT 30)

# DHCP server packet parser unit test (table-driven, ASan-instrumented)
add_test(
    NAME dhcp-server-process-unit
    COMMAND $<TARGET_FILE:test_dhcp_server_process>
)
set_tests_properties(dhcp-server-process-unit PROPERTIES TIMEOUT 30)

# DHCP server lifecycle / leak test (table-driven, ASan+LSan)
add_test(
    NAME dhcp-server-lifecycle-unit
    COMMAND $<TARGET_FILE:test_dhcp_server_lifecycle>
)
set_tests_properties(dhcp-server-lifecycle-unit PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

# Strict integer parser unit test (table-driven, ASan-instrumented)
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

# Loopback SGE-copy boundary-walking unit test
add_test(
    NAME loopback-sge-copy-unit
    COMMAND $<TARGET_FILE:test_loopback_sge_copy>
)
set_tests_properties(loopback-sge-copy-unit PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

# ionic MSI-X mask/pending unit test: asserts an assertion raised on a masked
# vector is latched and replayed on unmask rather than dropped.
add_test(
    NAME ionic-intr-pending-unit
    COMMAND $<TARGET_FILE:test_ionic_intr_pending>
)
set_tests_properties(ionic-intr-pending-unit PROPERTIES TIMEOUT 30)

# ionic QUERY_QP unit test: asserts the command writes the QP's real state
# into the side buffers the driver reads, rather than leaving them zeroed and
# reporting every queue pair as RESET.
add_test(
    NAME ionic-query-qp-unit
    COMMAND $<TARGET_FILE:test_ionic_query_qp>
)
set_tests_properties(ionic-query-qp-unit PROPERTIES TIMEOUT 30)

# ionic CQ arm unit test: asserts arming a CQ that still holds unreaped CQEs
# raises the event immediately, rather than waiting for a CQE that never comes.
add_test(
    NAME ionic-cq-arm-unit
    COMMAND $<TARGET_FILE:test_ionic_cq_arm>
)
set_tests_properties(ionic-cq-arm-unit PROPERTIES TIMEOUT 30)

# TCP mesh-topology broadcast unit test: asserts the unused node slots in the
# fixed-size payload are zeroed rather than carrying stack contents.
add_test(
    NAME tcp-mesh-topology-unit
    COMMAND $<TARGET_FILE:test_tcp_mesh_topology>
)
set_tests_properties(tcp-mesh-topology-unit PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

# TCP memory-region bounds unit test: asserts that remote addresses whose
# addr+len wraps 64 bits are rejected rather than resolving to a host pointer
# below the registered region.
add_test(
    NAME tcp-mr-bounds-unit
    COMMAND $<TARGET_FILE:test_tcp_mr_bounds>
)
set_tests_properties(tcp-mr-bounds-unit PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

# RDMA resource-table allocator unit test: asserts a full table refuses the
# next allocation rather than handing back a handle one past the end.
add_test(
    NAME rdma-res-tbl-unit
    COMMAND $<TARGET_FILE:test_rdma_res_tbl>
)
set_tests_properties(rdma-res-tbl-unit PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

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
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(ionic-ci PROPERTIES
    TIMEOUT 60
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
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
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
