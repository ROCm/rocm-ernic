# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: GPL-2.0-or-later

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
    ${IBVERBS_INCLUDE_DIRS}
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
    ${IBVERBS_INCLUDE_DIRS}
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
    ${CMAKE_SOURCE_DIR}/src/from-qemu
    ${CMAKE_SOURCE_DIR}/src/from-qemu/hw/rdma
    ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
    ${GLIB2_INCLUDE_DIRS}
    ${IBVERBS_INCLUDE_DIRS}
)
target_compile_definitions(test_rdma_backend_query_port PRIVATE
    _GNU_SOURCE TARGET_PAGE_SIZE=4096)
# -w: this test links the ported RDMA-backend translation units, so warnings
# would come from that ported C rather than the test itself; suppress them
# to match how the ported sources are compiled in the main build.
target_compile_options(test_rdma_backend_query_port PRIVATE
    -w ${ERNIC_UNIT_TEST_SAN})
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

# -- test_dc_loopback (requires libibverbs + installed rocm_ernic provider) -
# Only built when ERNIC_RDMA_CORE_BUILD=ON so that librocm_ernic and its
# installed headers (including infiniband/rocm_ernic_dc.h) are available.
if(ERNIC_RDMA_CORE_BUILD)
    add_executable(test_dc_loopback
        test_dc_loopback.c
    )
    target_compile_definitions(test_dc_loopback PRIVATE
        _POSIX_C_SOURCE=200809L
    )
    target_include_directories(test_dc_loopback PRIVATE
        ${IBVERBS_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/rdma-core/providers/rocm_ernic
        ${RDMA_CORE_INCLUDE_DIR}
    )
    target_link_directories(test_dc_loopback PRIVATE
        ${IBVERBS_LIBRARY_DIRS}
        ${RDMA_CORE_LIB_DIR}
    )
    target_link_libraries(test_dc_loopback PRIVATE
        ${IBVERBS_LIBRARIES}
        rocm_ernic
    )

    add_test(
        NAME dc-loopback-test
        COMMAND $<TARGET_FILE:test_dc_loopback>
    )
    set_tests_properties(dc-loopback-test PROPERTIES
        TIMEOUT 60
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
    )

    add_executable(test_dc_error_paths
        test_dc_error_paths.c
    )
    target_compile_definitions(test_dc_error_paths PRIVATE
        _POSIX_C_SOURCE=200809L
    )
    target_include_directories(test_dc_error_paths PRIVATE
        ${IBVERBS_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/rdma-core/providers/rocm_ernic
        ${RDMA_CORE_INCLUDE_DIR}
    )
    target_link_directories(test_dc_error_paths PRIVATE
        ${IBVERBS_LIBRARY_DIRS}
        ${RDMA_CORE_LIB_DIR}
    )
    target_link_libraries(test_dc_error_paths PRIVATE
        ${IBVERBS_LIBRARIES}
        rocm_ernic
    )

    add_test(
        NAME dc-error-paths-test
        COMMAND $<TARGET_FILE:test_dc_error_paths>
    )
    set_tests_properties(dc-error-paths-test PROPERTIES
        TIMEOUT 60
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
    )
endif()

# -- test_ernic_dc_uapi (cmocka + driver ABI header) --------------
if(CMOCKA_FOUND)
    add_executable(test_ernic_dc_uapi
        test_ernic_dc_uapi.c
    )
    target_include_directories(test_ernic_dc_uapi PRIVATE
        ${CMAKE_SOURCE_DIR}/driver
        ${CMOCKA_INCLUDE_DIRS}
    )
    target_link_directories(test_ernic_dc_uapi PRIVATE
        ${CMOCKA_LIBRARY_DIRS}
    )
    target_link_libraries(test_ernic_dc_uapi PRIVATE
        ${CMOCKA_LIBRARIES}
    )
endif()
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
    ${CMAKE_SOURCE_DIR}/src/from-qemu
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils
    ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
    ${GLIB2_INCLUDE_DIRS}
)
target_compile_definitions(test_rdma_cm_proto PRIVATE _GNU_SOURCE)
# QEMU-ported sources are compiled -w here, matching the main build.
target_compile_options(test_rdma_cm_proto PRIVATE
    -w ${ERNIC_PARSER_TEST_SAN})
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
    ${CMAKE_SOURCE_DIR}/src/from-qemu
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils
    ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
    ${GLIB2_INCLUDE_DIRS}
)
target_compile_definitions(test_dhcp_server_process PRIVATE _GNU_SOURCE)
# QEMU-ported sources are compiled -w here, matching the main build.
target_compile_options(test_dhcp_server_process PRIVATE
    -w ${ERNIC_PARSER_TEST_SAN})
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
    ${CMAKE_SOURCE_DIR}/src/from-qemu
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils
    ${CMAKE_SOURCE_DIR}/src/from-qemu/include/qemu-extra
    ${GLIB2_INCLUDE_DIRS}
)
target_compile_definitions(test_dhcp_server_lifecycle PRIVATE _GNU_SOURCE)
# QEMU-ported sources are compiled -w here, matching the main build.
target_compile_options(test_dhcp_server_lifecycle PRIVATE
    -w ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_dhcp_server_lifecycle PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})
target_link_directories(test_dhcp_server_lifecycle PRIVATE
    ${GLIB2_LIBRARY_DIRS})
target_link_libraries(test_dhcp_server_lifecycle PRIVATE ${GLIB2_LIBRARIES})

# ROCM_ERNIC_ETH_CTL_RESET bit-definition test (header-only, project header
# so warnings stay enabled).
add_executable(test_eth_ctl_reset test_eth_ctl_reset.c)
target_include_directories(test_eth_ctl_reset PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_eth_ctl_reset PRIVATE ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_eth_ctl_reset PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})

# Strict integer parser (parse_int.h) unit test (header-only). The ported
# header dir is a SYSTEM include so its warnings stay quiet while the test
# source itself is still warning-checked.
add_executable(test_parse_int test_parse_int.c)
target_include_directories(test_parse_int SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/src/from-qemu/utils
)
target_compile_options(test_parse_int PRIVATE ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_parse_int PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})

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
    _GNU_SOURCE TARGET_PAGE_SIZE=4096)
# -w: this test #includes the ported loopback translation unit, so warnings
# would come from that ported C rather than the test itself; suppress them
# to match how the ported sources are compiled in the main build.
target_compile_options(test_loopback_sge_copy PRIVATE
    -w ${ERNIC_UNIT_TEST_SAN})
target_link_options(test_loopback_sge_copy PRIVATE ${ERNIC_UNIT_TEST_SAN_LINK})
target_link_directories(test_loopback_sge_copy PRIVATE
    ${GLIB2_LIBRARY_DIRS} ${IBVERBS_LIBRARY_DIRS})
target_link_libraries(test_loopback_sge_copy PRIVATE
    ${GLIB2_LIBRARIES} ${IBVERBS_LIBRARIES})

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

# Data transfer test (may need RDMA device / loopback backend)
add_test(
    NAME data-transfer-test
    COMMAND $<TARGET_FILE:test_data_transfer>
)
set_tests_properties(data-transfer-test PROPERTIES
    TIMEOUT 60
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
)

# Loopback WRITE_WITH_IMM responder-delivery test
add_test(
    NAME write-imm-test
    COMMAND $<TARGET_FILE:test_write_imm>
)
set_tests_properties(write-imm-test PROPERTIES
    TIMEOUT 60
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
)

# RDMA Connection Manager emulation test
add_test(
    NAME rdma-cm-test
    COMMAND $<TARGET_FILE:test_rdma_cm>
)
set_tests_properties(rdma-cm-test PROPERTIES
    TIMEOUT 60
    RUN_SERIAL TRUE
    SKIP_RETURN_CODE 77
)

# rdma_backend_query_port NULL-guard / dispatch unit test
add_test(
    NAME rdma-backend-query-port-unit
    COMMAND $<TARGET_FILE:test_rdma_backend_query_port>
)
set_tests_properties(rdma-backend-query-port-unit PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

if(CMOCKA_FOUND)
    add_test(
        NAME ernic_dc_uapi
        COMMAND $<TARGET_FILE:test_ernic_dc_uapi>
    )
    set_tests_properties(ernic_dc_uapi PROPERTIES
        TIMEOUT 30
        RUN_SERIAL TRUE
    )
endif()
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

# Ethernet control reset-bit definition unit test
add_test(
    NAME eth-ctl-reset-unit
    COMMAND $<TARGET_FILE:test_eth_ctl_reset>
)
set_tests_properties(eth-ctl-reset-unit PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

# Strict integer parser unit test (table-driven, ASan-instrumented)
add_test(
    NAME parse-int-unit
    COMMAND $<TARGET_FILE:test_parse_int>
)
set_tests_properties(parse-int-unit PROPERTIES TIMEOUT 30)

# Loopback SGE-copy boundary-walking unit test
add_test(
    NAME loopback-sge-copy-unit
    COMMAND $<TARGET_FILE:test_loopback_sge_copy>
)
set_tests_properties(loopback-sge-copy-unit PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
