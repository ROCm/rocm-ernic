/*
 * An RDMA policy that converts: a table keyed on the operation's opcode
 * turns an ordinary RDMA write into an elementwise format conversion, and
 * the table entry names both formats -- so the same program narrows
 * (fp32 to fp4, say) or widens (fp4 to fp16) depending only on what the
 * control plane put in the table.
 *
 * This is the other shape the extern has.  fp8_policy.p4 calls map(),
 * which combines two equal-length operands into a result of the same
 * length; this calls cvt(), which reads ONE operand and writes a result
 * that may be longer, shorter or the same length.  Which one a work
 * request is comes from its gather list -- two equal entries is a
 * reduction, one entry is a conversion -- so the same device serves both
 * without the program naming any memory.
 *
 * The format numbers are the enum p4_fmt values in src/p4/p4_fmt.h:
 * 0 fp32, 1 fp16, 2 bf16, 3 fp8 E4M3, 4 fp8 E5M2, 5 fp4 E2M1.
 *
 * P4 has no floating-point type at all, so neither the arithmetic nor the
 * conversion can be written in the language; both are externs the target
 * provides.  What the program decides is the policy: whether to convert,
 * and between which formats.
 *
 * Compile with:
 *   scripts/fetch-p4c.sh
 *   build-p4c/bin/p4c-bm2-ss --std p4-16 --emit-externs \
 *       -I build-p4c/p4include \
 *       -o tests/data/p4/fp8_convert.json \
 *       tests/data/p4/src/fp8_convert.p4
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
    /*
     * The extern writes the destination itself, so the operation must not
     * also transfer: the ordinary RDMA write would land second and put
     * the raw source payload where the converted result belongs.  The
     * device refuses an operation whose program called the extern without
     * dropping, rather than letting that happen quietly.
     */
    action convert(bit<8> src_fmt, bit<8> dst_fmt) {
        fp8.cvt(src_fmt, dst_fmt);
        mark_to_drop(s);
    }
    action passthrough() { }
    table policy {
        key = { m.rdma_metadata.opcode : exact; }
        actions = { convert; passthrough; }
        default_action = passthrough();
        size = 64;
    }
    apply { policy.apply(); }
}
control Eg(inout headers_t h, inout metadata_t m, inout standard_metadata_t s) { apply { } }
control CC(inout headers_t h, inout metadata_t m) { apply { } }
control D(packet_out p, in headers_t h) { apply { } }
V1Switch(P(), VC(), Ing(), Eg(), CC(), D()) main;
