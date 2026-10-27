/*
 * Two tables at the edges of what a key can be.
 *
 * by_type matches `etherType & 0x00ff`, which p4c writes as the whole
 * field with a mask. bmv2 puts both the packet's key and each entry's
 * under the mask before they meet, so an entry for 0x0034 takes 0x1234
 * and 0xab34 alike, and one for 0x1234 is the same row.
 *
 * by_addr matches a 128-bit field, the widest an entry can carry, and its
 * action takes a 128-bit argument, which it writes back into that field.
 *
 * Compile with:
 *   scripts/fetch-p4c.sh
 *   build-p4c/bin/p4c-bm2-ss --std p4-16 -I build-p4c/p4include \
 *       -o tests/data/p4/key_mask.json \
 *       tests/data/p4/src/key_mask.p4
 *
 * Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <core.p4>
#include <v1model.p4>

header ethernet_t {
    bit<48> dstAddr;
    bit<48> srcAddr;
    bit<16> etherType;
}
header addr_t { bit<128> addr; }

struct headers_t {
    ethernet_t ethernet;
    addr_t     a;
}
struct metadata_t { }

parser ParserImpl(packet_in packet, out headers_t hdr,
                  inout metadata_t meta, inout standard_metadata_t smeta) {
    state start {
        packet.extract(hdr.ethernet);
        packet.extract(hdr.a);
        transition accept;
    }
}

control VerifyChecksumImpl(inout headers_t hdr, inout metadata_t meta) {
    apply { }
}

control IngressImpl(inout headers_t hdr, inout metadata_t meta,
                    inout standard_metadata_t smeta) {
    action set_egress(bit<9> port) { smeta.egress_spec = port; }
    action drop_pkt() { mark_to_drop(smeta); }
    action set_addr(bit<128> v) { hdr.a.addr = v; }

    table by_type {
        key = { hdr.ethernet.etherType & 0x00ff : exact; }
        actions = { set_egress; drop_pkt; }
        default_action = drop_pkt();
        size = 16;
    }
    table by_addr {
        key = { hdr.a.addr : exact; }
        actions = { set_addr; NoAction; }
        default_action = NoAction();
        size = 16;
    }

    apply {
        by_addr.apply();
        by_type.apply();
    }
}

control EgressImpl(inout headers_t hdr, inout metadata_t meta,
                   inout standard_metadata_t smeta) {
    apply { }
}

control ComputeChecksumImpl(inout headers_t hdr, inout metadata_t meta) {
    apply { }
}

control DeparserImpl(packet_out packet, in headers_t hdr) {
    apply {
        packet.emit(hdr.ethernet);
        packet.emit(hdr.a);
    }
}

V1Switch(ParserImpl(), VerifyChecksumImpl(), IngressImpl(), EgressImpl(),
         ComputeChecksumImpl(), DeparserImpl()) main;
