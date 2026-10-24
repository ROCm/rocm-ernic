/*
 * An action that calls the extern twice: a conversion, and then an
 * arithmetic the request cannot be.
 *
 * It exists to be refused. The first call writes the destination; the
 * second declines, because a conversion's work request has one gather
 * entry and a reduction needs two. Letting the operation through at that
 * point would dispatch the ordinary RDMA write over the converted result
 * and report success, so `ionic_datapath.c` fails it instead: once an
 * extern has written the destination, nothing may transfer on top of it.
 *
 * A program nobody would write deliberately, but one a guest may write,
 * and the device is what has to hold the line.
 *
 * Compile with:
 *   scripts/fetch-p4c.sh
 *   build-p4c/bin/p4c-bm2-ss --std p4-16 --emit-externs \
 *       -I build-p4c/p4include \
 *       -o tests/data/p4/fp8_cvt_then_map.json \
 *       tests/data/p4/src/fp8_cvt_then_map.p4
 *
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <core.p4>
#include <v1model.p4>

extern Fp8 {
    Fp8();
    void map(in bit<8> op);
    void cvt(in bit<8> src_fmt, in bit<8> dst_fmt);
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
    action convert_then_reduce(bit<8> src_fmt, bit<8> dst_fmt, bit<8> op) {
        fp8.cvt(src_fmt, dst_fmt);
        fp8.map(op);
        mark_to_drop(s);
    }
    action passthrough() { }
    table policy {
        key = { m.rdma_metadata.opcode : exact; }
        actions = { convert_then_reduce; passthrough; }
        default_action = passthrough();
        size = 64;
    }
    apply { policy.apply(); }
}
control Eg(inout headers_t h, inout metadata_t m, inout standard_metadata_t s) { apply { } }
control CC(inout headers_t h, inout metadata_t m) { apply { } }
control D(packet_out p, in headers_t h) { apply { } }
V1Switch(P(), VC(), Ing(), Eg(), CC(), D()) main;
