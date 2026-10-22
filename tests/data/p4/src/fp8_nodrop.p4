/*
 * The same policy as fp8_policy.p4, with the drop left out of the action
 * that calls the extern -- a program written the way one plausibly would
 * be on the first attempt.
 *
 * The extern writes the destination itself, so an operation that then also
 * transfers would overwrite the result with the raw payload.  The device
 * refuses such an operation, and this is the program that makes it do so.
 * It exists to be rejected.  The arithmetic is an extern the
 * target provides, because P4 has no floating-point type -- its base types
 * are bit-vectors -- so fp8 cannot be expressed in the language itself.
 *
 * What the program decides is the policy: whether to act, and with which
 * operation.  What it cannot decide is which memory is touched; the
 * operand regions come from the work request the guest posted, not from
 * anything a table entry can name.
 *
 * Compile with:
 *   scripts/fetch-p4c.sh
 *   build-p4c/bin/p4c-bm2-ss --std p4-16 --emit-externs \
 *       -I build-p4c/p4include \
 *       -o tests/data/p4/fp8_nodrop.json \
 *       tests/data/p4/src/fp8_nodrop.p4
 *
 * --emit-externs is required: without it p4c refuses an extern it does
 * not already know.
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
    action reduce_with(bit<8> op) { fp8.map(op); }
    action passthrough() { }
    table policy {
        key = { m.rdma_metadata.opcode : exact; }
        actions = { reduce_with; passthrough; }
        default_action = passthrough();
        size = 64;
    }
    apply { policy.apply(); }
}
control Eg(inout headers_t h, inout metadata_t m, inout standard_metadata_t s) { apply { } }
control CC(inout headers_t h, inout metadata_t m) { apply { } }
control D(packet_out p, in headers_t h) { apply { } }
V1Switch(P(), VC(), Ing(), Eg(), CC(), D()) main;
