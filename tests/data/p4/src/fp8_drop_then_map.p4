/*
 * An action that drops the operation and then calls the extern.
 *
 * It exists to show that a drop made before the extern declines still
 * stands. A one-SGE RDMA write is not a reduction's shape, so map()
 * declines it and the pipeline stops at the call. The decline decides
 * nothing about the operation, but the mark_to_drop() ahead of it did,
 * and `ionic_datapath.c` honours that drop rather than dispatching the
 * ordinary write the program denied.
 *
 * The drop sits in the same action to keep the program small; one in an
 * earlier table, such as an access list in front of the compute table,
 * reaches the same code.
 *
 * Compile with:
 *   scripts/fetch-p4c.sh
 *   build-p4c/bin/p4c-bm2-ss --std p4-16 --emit-externs \
 *       -I build-p4c/p4include \
 *       -o tests/data/p4/fp8_drop_then_map.json \
 *       tests/data/p4/src/fp8_drop_then_map.p4
 *
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <core.p4>
#include <v1model.p4>

extern Fp8 {
    Fp8();
    void map(in bit<8> op);
}

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
    Fp8() fp8;
    action drop_then_reduce(bit<8> op) {
        mark_to_drop(s);
        fp8.map(op);
    }
    action passthrough() { }
    table policy {
        key = { m.rdma_metadata.opcode : exact; }
        actions = { drop_then_reduce; passthrough; }
        default_action = passthrough();
        size = 64;
    }
    apply { policy.apply(); }
}
control Eg(inout headers_t h, inout metadata_t m, inout standard_metadata_t s) { apply { } }
control CC(inout headers_t h, inout metadata_t m) { apply { } }
control D(packet_out p, in headers_t h) { apply { } }
V1Switch(P(), VC(), Ing(), Eg(), CC(), D()) main;
