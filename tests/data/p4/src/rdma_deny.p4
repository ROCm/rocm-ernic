/*
 * An RDMA-path access list: a table keyed on the opcode whose deny()
 * action drops the operation and calls nothing else.
 *
 * It exists to show what a denied operation completes with. Nothing is
 * transferred, and the work request completes with a local QP operation
 * error, on every transport, so the sender is not told that an operation
 * the program refused was done.
 *
 * Compile with:
 *   scripts/fetch-p4c.sh
 *   build-p4c/bin/p4c-bm2-ss --std p4-16 -I build-p4c/p4include \
 *       -o tests/data/p4/rdma_deny.json \
 *       tests/data/p4/src/rdma_deny.p4
 *
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <core.p4>
#include <v1model.p4>

header rdma_t {
    bit<8>  opcode;
    bit<24> qp_num;
    bit<32> length;
    bit<32> imm_data;
    bit<32> rkey;
}
struct headers_t { }
struct metadata_t { rdma_t rdma_metadata; }

parser P(packet_in p, out headers_t h, inout metadata_t m,
         inout standard_metadata_t s) { state start { transition accept; } }
control VC(inout headers_t h, inout metadata_t m) { apply { } }
control Ing(inout headers_t h, inout metadata_t m, inout standard_metadata_t s) {
    action deny() { mark_to_drop(s); }
    action allow() { }
    table acl {
        key = { m.rdma_metadata.opcode : exact; }
        actions = { deny; allow; }
        default_action = allow();
        size = 64;
    }
    apply { acl.apply(); }
}
control Eg(inout headers_t h, inout metadata_t m, inout standard_metadata_t s) { apply { } }
control CC(inout headers_t h, inout metadata_t m) { apply { } }
control D(packet_out p, in headers_t h) { apply { } }
V1Switch(P(), VC(), Ing(), Eg(), CC(), D()) main;
