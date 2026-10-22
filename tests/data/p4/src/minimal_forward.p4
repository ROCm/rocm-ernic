/*
 * The smallest program that forwards: parse Ethernet, match on the
 * destination address, set the egress port.  It exists so the loader and
 * the executor are tested against something a reader can hold in their
 * head, and against real compiler output rather than hand-written JSON.
 *
 * Compile with:
 *   scripts/fetch-p4c.sh
 *   build-p4c/bin/p4c-bm2-ss --std p4-16 -I build-p4c/p4include \
 *       -o tests/data/p4/minimal_forward.json \
 *       tests/data/p4/src/minimal_forward.p4
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

struct headers_t { ethernet_t ethernet; }
struct metadata_t { }

parser ParserImpl(packet_in packet, out headers_t hdr,
                  inout metadata_t meta, inout standard_metadata_t smeta) {
    state start {
        packet.extract(hdr.ethernet);
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

    table forward {
        key = { hdr.ethernet.dstAddr : exact; }
        actions = { set_egress; drop_pkt; }
        /* No entries are installed by default, so every packet misses and
         * is dropped until a control plane says otherwise. */
        default_action = drop_pkt();
        size = 1024;
    }

    apply { forward.apply(); }
}

control EgressImpl(inout headers_t hdr, inout metadata_t meta,
                   inout standard_metadata_t smeta) {
    apply { }
}

control ComputeChecksumImpl(inout headers_t hdr, inout metadata_t meta) {
    apply { }
}

control DeparserImpl(packet_out packet, in headers_t hdr) {
    apply { packet.emit(hdr.ethernet); }
}

V1Switch(ParserImpl(), VerifyChecksumImpl(), IngressImpl(), EgressImpl(),
         ComputeChecksumImpl(), DeparserImpl()) main;
