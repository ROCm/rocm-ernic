# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""Mutation check for the Rx header-split work: each mutant must fail ionic-eth-rx-unit.

Works on a snapshot of the tree (never the tree itself), with its own build
directory.  Usage: mutate.py TREE SCRATCH
"""

import os
import subprocess
import sys
from pathlib import Path

tree, scratch = sys.argv[1], sys.argv[2]
snap = os.path.join(scratch, "src")
build = os.path.join(scratch, "build")
if not os.path.isdir(snap):
    os.makedirs(snap)
    files = subprocess.run(
        [
            "git",
            "-C",
            tree,
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
        ],
        check=True,
        capture_output=True,
        env=dict(os.environ, GIT_OPTIONAL_LOCKS="0"),
    ).stdout
    subprocess.run(
        [
            "tar",
            "--null",
            "-T",
            "-",
            "--ignore-failed-read",
            "-cf",
            os.path.join(scratch, "snap.tar"),
        ],
        input=files,
        cwd=tree,
        check=True,
    )
    subprocess.run(
        ["tar", "-xf", os.path.join(scratch, "snap.tar"), "-C", snap], check=True
    )
    subprocess.run(
        [
            "cmake",
            "-S",
            snap,
            "-B",
            build,
            "-G",
            "Ninja",
            "-DCMAKE_C_COMPILER=gcc-13",
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DERNIC_BUILD_P4=ON",
            "-DERNIC_USE_SANITIZERS=ON",
        ],
        check=True,
        capture_output=True,
    )

RX = "src/ionic/ionic_eth_rx.c"
EMU = "src/ionic/ionic_eth_emu.c"
M = [
    (
        "mode ignored: never parse",
        EMU,
        "    if (emu->profile.rx.header_split) {\n        ionic_eth_rx_parse(",
        "    if (false) {\n        ionic_eth_rx_parse(",
    ),
    ("never split", EMU, "q->hds && info.splittable ? info.hdr_len : 0", "0"),
    (
        "split on queues that did not ask",
        EMU,
        "q->hds && info.splittable ? info.hdr_len : 0",
        "info.splittable ? info.hdr_len : 0",
    ),
    (
        "split SIMPLE descriptors",
        EMU,
        "bool split = hdr_len != 0 && opcode == RXQ_DESC_OPCODE_SG;",
        "bool split = hdr_len != 0;",
    ),
    ("no HBO flag", EMU, "        *hds_flags = RXQ_COMP_HDS_F_HBO;\n", ""),
    (
        "HBO off by one",
        EMU,
        "if (split && hdr_len > bufs[0].len) {",
        "if (split && hdr_len >= bufs[0].len) {",
    ),
    (
        "headers-only frame reads the SG list",
        EMU,
        "    if (split && hdr_len == len) {",
        "    if (false) {",
    ),
    (
        "no prefix fallback when the frame fits the first buffer",
        EMU,
        "        if (len <= bufs[0].len) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "\n"
        "    int n = eth_rx_sg_bufs",
        "    }\n\n    int n = eth_rx_sg_bufs",
    ),
    (
        "split leaves buffer 0 at its full length",
        EMU,
        "            bufs[0].len = (uint16_t)hdr_len;\n",
        "",
    ),
    (
        "zero-length element does not end the list",
        EMU,
        "        if (!elen) {\n            break;\n        }\n        n++;",
        "        if (!elen) {\n            continue;\n        }\n        n++;",
    ),
    (
        "16-byte entries on an HDS queue",
        EMU,
        "        entry_size += RXQ_COMP_EXT_SIZE;\n",
        "",
    ),
    (
        "base completion not moved behind the extension",
        EMU,
        "        comp += RXQ_COMP_EXT_SIZE;\n",
        "",
    ),
    (
        "hds_hdr_len = desc.len",
        EMU,
        "size_t head = len < bufs[0].len ? len : bufs[0].len;",
        "size_t head = bufs[0].len;",
    ),
    (
        "no hds_flags in the entry",
        EMU,
        "        entry[RXQ_COMP_EXT_HDS_FLAGS_OFF] = hds_flags;\n",
        "",
    ),
    (
        "csum little-endian",
        EMU,
        "ernic_put_be16(comp + RXQ_COMP_CSUM_OFF, info.csum);",
        "ernic_put_le16(comp + RXQ_COMP_CSUM_OFF, info.csum);",
    ),
    (
        "no csum_flags",
        EMU,
        "    comp[RXQ_COMP_CSUM_FLAGS_OFF] = info.csum_flags;\n",
        "",
    ),
    (
        "RX_CSUM always granted",
        EMU,
        "(emu->profile.rx.header_split ? IONIC_ETH_HW_RX_CSUM : 0)",
        "IONIC_ETH_HW_RX_CSUM",
    ),
    (
        "RX_CSUM never granted",
        EMU,
        "(emu->profile.rx.header_split ? IONIC_ETH_HW_RX_CSUM : 0)",
        "0",
    ),
    (
        "Q_IDENTIFY features for every qtype",
        EMU,
        "qtype == IONIC_QTYPE_RXQ && emu->profile.rx.header_split\n"
        "                       ? IONIC_RXQ_FEATURES_HDS",
        "emu->profile.rx.header_split\n                       ? IONIC_RXQ_FEATURES_HDS",
    ),
    (
        "Q_IDENTIFY features in mode off",
        EMU,
        "qtype == IONIC_QTYPE_RXQ && emu->profile.rx.header_split\n"
        "                       ? IONIC_RXQ_FEATURES_HDS",
        "qtype == IONIC_QTYPE_RXQ\n                       ? IONIC_RXQ_FEATURES_HDS",
    ),
    (
        "Q_INIT features read in mode off",
        EMU,
        "qtype == IONIC_QTYPE_RXQ && emu->profile.rx.header_split;",
        "qtype == IONIC_QTYPE_RXQ;",
    ),
    (
        "Q_INIT features read for every qtype",
        EMU,
        "qtype == IONIC_QTYPE_RXQ && emu->profile.rx.header_split;",
        "emu->profile.rx.header_split;",
    ),
    (
        "Q_INIT: unknown bits not refused",
        EMU,
        "(features & ~IONIC_RXQ_FEATURES_HDS) != 0",
        "false",
    ),
    (
        "Q_INIT: half a pair accepted",
        EMU,
        "features != 0 && (features != IONIC_RXQ_FEATURES_HDS || !sg)",
        "features != 0 && !sg",
    ),
    (
        "Q_INIT: HDS without SG accepted",
        EMU,
        "features != 0 && (features != IONIC_RXQ_FEATURES_HDS || !sg)",
        "features != 0 && features != IONIC_RXQ_FEATURES_HDS",
    ),
    (
        "Q_INIT: hds from one bit",
        EMU,
        ".hds = features == IONIC_RXQ_FEATURES_HDS,",
        ".hds = false,",
    ),
    (
        "short frame not refused",
        RX,
        "    if (len < ETH_HDR_LEN) {\n        return;\n    }",
        "",
    ),
    (
        "no CALC",
        RX,
        "info->csum_flags = IONIC_RXQ_CSUM_F_CALC;",
        "info->csum_flags = 0;",
    ),
    (
        "sum from byte 0",
        RX,
        "net_csum_add(0, frame + ETH_HDR_LEN, len - ETH_HDR_LEN)",
        "net_csum_add(0, frame, len)",
    ),
    (
        "three VLAN tags",
        RX,
        "#define RX_VLAN_TAGS_MAX 2U",
        "#define RX_VLAN_TAGS_MAX 3U",
    ),
    ("one VLAN tag", RX, "#define RX_VLAN_TAGS_MAX 2U", "#define RX_VLAN_TAGS_MAX 1U"),
    (
        "truncated tag not refused",
        RX,
        "        if (len - l3 < ETH_VLAN_TAG_LEN) {\n            return;\n        }\n",
        "",
    ),
    (
        "IPv4 header may be short",
        RX,
        "    if (len - l3 < IP_HDR_LEN) {\n"
        "        return false;\n"
        "    }\n"
        "\n"
        "    const uint8_t *ip = frame + l3;\n"
        "    size_t ihl",
        "    const uint8_t *ip = frame + l3;\n    size_t ihl",
    ),
    (
        "IPv4 version not checked",
        RX,
        "ip[IP_VERSION_IHL_OFF] >> IP_VERSION_SHIFT != IP_VERSION ||\n        ihl < IP_HDR_LEN",
        "ihl < IP_HDR_LEN",
    ),
    (
        "ihl below 20 accepted",
        RX,
        "ihl < IP_HDR_LEN || ihl > len - l3",
        "ihl > len - l3",
    ),
    (
        "ihl past the frame accepted",
        RX,
        "ihl < IP_HDR_LEN || ihl > len - l3",
        "ihl < IP_HDR_LEN",
    ),
    (
        "IPv4 checksum not checked",
        RX,
        "if (net_csum_fold(net_csum_add(0, ip, ihl)) != 0) {",
        "if (false) {",
    ),
    (
        "IP_BAD goes on to L4",
        RX,
        "        *flags |= IONIC_RXQ_CSUM_F_IP_BAD;\n        return false;",
        "        *flags |= IONIC_RXQ_CSUM_F_IP_BAD;",
    ),
    (
        "fragments not refused",
        RX,
        "(ernic_get_be16(ip + IP_FRAGMENT_OFF) & IP_FRAGMENT_MASK) != 0 ||\n        total < ihl",
        "total < ihl",
    ),
    (
        "only fragment offset checked, not MF",
        RX,
        '#include "net/net_headers.h"\n',
        '#include "net/net_headers.h"\n#undef IP_FRAGMENT_MASK\n#define IP_FRAGMENT_MASK 0x1fff\n',
    ),
    (
        "total below ihl accepted",
        RX,
        "total < ihl || total > len - l3",
        "total > len - l3",
    ),
    (
        "total past the frame accepted",
        RX,
        "total < ihl || total > len - l3",
        "total < ihl",
    ),
    (
        "IPv6 header may be short",
        RX,
        "    if (len - l3 < IP6_HDR_LEN) {\n        return false;\n    }\n",
        "",
    ),
    (
        "IPv6 version not checked",
        RX,
        "ip[0] >> IP6_VERSION_SHIFT != IP6_VERSION ||\n        payload",
        "payload",
    ),
    (
        "IPv6 payload past the frame accepted",
        RX,
        "        payload > len - l3 - IP6_HDR_LEN) {",
        "        false) {",
    ),
    ("UDP shorter than its header judged", RX, "l4.len >= UDP_HDR_LEN &&\n", ""),
    (
        "UDP length mismatch judged",
        RX,
        "            ernic_get_be16(hdr + UDP_LEN_OFF) == l4.len &&\n",
        "",
    ),
    (
        "zero UDP checksum judged",
        RX,
        " &&\n            ernic_get_be16(hdr + UDP_CHECKSUM_OFF) != 0) {",
        ") {",
    ),
    (
        "UDP always OK",
        RX,
        "info->csum_flags |= rx_l4_csum_ok(frame, &l4)\n",
        "info->csum_flags |= true\n",
    ),
    (
        "TCP shorter than its header judged",
        RX,
        "l4.protocol != IP_PROTOCOL_TCP || l4.len < TCP_HDR_LEN",
        "l4.protocol != IP_PROTOCOL_TCP",
    ),
    (
        "any protocol treated as TCP",
        RX,
        "l4.protocol != IP_PROTOCOL_TCP || l4.len < TCP_HDR_LEN",
        "l4.len < TCP_HDR_LEN",
    ),
    (
        "TCP checksum not checked",
        RX,
        "    if (!rx_l4_csum_ok(frame, &l4)) {\n"
        "        info->csum_flags |= IONIC_RXQ_CSUM_F_TCP_BAD;",
        "    if (false) {\n        info->csum_flags |= IONIC_RXQ_CSUM_F_TCP_BAD;",
    ),
    (
        "TCP_BAD still split",
        RX,
        "        info->csum_flags |= IONIC_RXQ_CSUM_F_TCP_BAD;\n        return;",
        "        info->csum_flags |= IONIC_RXQ_CSUM_F_TCP_BAD;",
    ),
    (
        "padded frames split",
        RX,
        "l4.off + l4.len != len || tcp_hdr_len < TCP_HDR_LEN",
        "tcp_hdr_len < TCP_HDR_LEN",
    ),
    (
        "TCP header below 20 split",
        RX,
        "l4.off + l4.len != len || tcp_hdr_len < TCP_HDR_LEN ||",
        "l4.off + l4.len != len ||",
    ),
    (
        "TCP header past the segment split",
        RX,
        " ||\n        tcp_hdr_len > l4.len) {",
        ") {",
    ),
    (
        "pseudo-header without the length",
        RX,
        "l4->pseudo + l4->protocol + (uint32_t)l4->len;",
        "l4->pseudo + l4->protocol;",
    ),
    (
        "pseudo-header without the protocol",
        RX,
        "l4->pseudo + l4->protocol + (uint32_t)l4->len;",
        "l4->pseudo + (uint32_t)l4->len;",
    ),
    (
        "hdr_len without TCP options",
        RX,
        "info->hdr_len = (uint16_t)(l4.off + tcp_hdr_len);",
        "info->hdr_len = (uint16_t)(l4.off + TCP_HDR_LEN);",
    ),
    (
        "IPv6 extension headers treated as TCP",
        RX,
        "    l4->protocol = ip[IP6_NEXT_HDR_OFF];",
        "    l4->protocol = IP_PROTOCOL_TCP;",
    ),
]
survived = 0
for name, path, old, new in M:
    full = Path(snap, path)
    orig = full.read_text(encoding="utf-8")
    if orig.count(old) != 1:
        print(f"SETUP  {name}: pattern found {orig.count(old)} times")
        survived += 1
        continue
    full.write_text(orig.replace(old, new), encoding="utf-8")
    try:
        b = subprocess.run(
            ["cmake", "--build", build, "--target", "test_ionic_eth_rx"],
            capture_output=True,
            text=True,
            check=False,
        )
        if b.returncode != 0:
            print(f"NOBUILD {name}")
            continue
        r = subprocess.run(
            [os.path.join(build, "tests/test_ionic_eth_rx")],
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        fails = r.stdout.count("\nFAIL") + r.stdout.startswith("FAIL")
        if r.returncode == 0:
            print(f"SURVIVED {name}")
            survived += 1
        elif fails:
            print(f"killed  {name}: {fails} rows fail")
        else:
            how = (
                "ASan/UBSan"
                if "Sanitizer" in r.stderr or "runtime error" in r.stderr
                else "crash"
            )
            print(f"killed  {name}: exit {r.returncode} ({how})")
    finally:
        full.write_text(orig, encoding="utf-8")
print(f"{len(M)} mutants, {survived} survived or not applied")
