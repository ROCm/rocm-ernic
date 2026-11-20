#!/usr/bin/env python3
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
"""Host-side driver for the emulator TX-offload validation matrix.

run.py LABEL [CONFIG[:FAMILY]...]    LABEL names the evidence subdirectory
                                     (e.g. k7215-dkms). No CONFIG = all.

One "suite" = one offload configuration x one address family, traffic in
both directions. Evidence: evidence/emu-offloads/LABEL/<config>-<family>.json
(+ decoded captures), one line per suite in LABEL/results.tsv.
"""

import gzip
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

if not os.environ.get("DEVMEM_ROOT"):
    sys.exit("run.py: set DEVMEM_ROOT to the work area")
T = Path(os.environ["DEVMEM_ROOT"]) / "test"
GSSH = str(T / "scripts/gssh")
GT = "/mnt/hostfs/test/scripts/emu-offloads/gt.py"
EV = T / "evidence/emu-offloads"
DEV = "rocm-ernic0"
MAC = {1: "72:6f:63:6d:00:01", 2: "72:6f:63:6d:00:02"}
PORT = 5400

FAMILIES = {
    "v4": {"addr": lambda n: f"192.168.100.1{n}", "ip_hdr": 20, "vlan": 0},
    "v6": {"addr": lambda n: f"fd00:100::1{n}", "ip_hdr": 40, "vlan": 0},
    "vlan4": {"addr": lambda n: f"192.168.101.1{n}", "ip_hdr": 20, "vlan": 4},
    "vlan6": {"addr": lambda n: f"fd00:101::1{n}", "ip_hdr": 40, "vlan": 4},
}

# name -> (mtu, commands after the reset, expectations on sender counters)
CONFIGS = {
    "default": (1500, "", {"tso": True, "csum": True}),
    "gso-off": (1500, "sudo ethtool -K $D gso off", {"tso": True, "csum": True}),
    "tso-off": (1500, "sudo ethtool -K $D tso off", {"tso": False, "csum": True}),
    "tso-gso-off": (
        1500,
        "sudo ethtool -K $D tso off gso off",
        {"tso": False, "csum": True},
    ),
    "sg-off": (1500, "sudo ethtool -K $D sg off", {"tso": False, "csum": True}),
    "tx-off": (1500, "sudo ethtool -K $D tx off", {"tso": False, "csum": False}),
    "ecn": (1500, "sudo sysctl -qw net.ipv4.tcp_ecn=1", {"tso": True, "csum": True}),
    # CE-mark every data packet at the sender's qdisc so the peer echoes ECE
    # and the sender puts CWR on TSO skbs (SKB_GSO_TCP_ECN). Needs fq_codel.
    "ecn-ce": (
        1500,
        "sudo sysctl -qw net.ipv4.tcp_ecn=1; "
        "sudo tc qdisc replace dev $D root fq_codel ce_threshold 1us || exit 1; "
        "tc qdisc show dev $D | head -1",
        {"tso": True, "csum": True},
    ),
    "mtu9000": (9000, "", {"tso": True, "csum": True}),
    "mtu9198": (9198, "", {"tso": True, "csum": True}),
    "mtu9000-tso-off": (
        9000,
        "sudo ethtool -K $D tso off",
        {"tso": False, "csum": True},
    ),
}
DEFAULT_PLAN = [f"{c}:{f}" for c in CONFIGS if c != "ecn-ce" for f in ("v4", "v6")]

RESET = r"""
set -u
D=rocm-ernic0
sudo ethtool -K $D tx on; sudo ethtool -K $D sg on; sudo ethtool -K $D tso on gso on gro on
sudo sysctl -qw net.ipv4.tcp_ecn=2
sudo tc qdisc del dev $D root 2>/dev/null
sudo ip link del ernic.100 2>/dev/null
sudo ip link set $D mtu @MTU@
sudo ip -6 addr replace fd00:100::1@N@/64 dev $D nodad
if [ @VLAN@ = 1 ]; then
    sudo ip link add link $D name ernic.100 type vlan id 100 || exit 1
    sudo ip link set ernic.100 mtu @MTU@ up
    sudo ip addr replace 192.168.101.1@N@/24 dev ernic.100
    sudo ip -6 addr replace fd00:101::1@N@/64 dev ernic.100 nodad
    ip -o link show ernic.100 | cut -d' ' -f2-5
    ethtool -k ernic.100 | grep -E '^(tx-checksumming|scatter-gather|tcp-segmentation-offload):'
fi
sudo ip tcp_metrics flush >/dev/null 2>&1
@EXTRA@
echo "== $(uname -r) ionic srcversion $(cat /sys/module/ionic/srcversion 2>/dev/null) rdma=$(test -e /sys/module/ionic_rdma && echo yes || echo no)"
ip -o link show $D | cut -d' ' -f2-5
ethtool -k $D | grep -E '^(tx-checksumming|scatter-gather|tcp-segmentation-offload|generic-segmentation-offload|generic-receive-offload):|tx-checksum-ip-generic|tx-scatter-gather:|tx-tcp-segmentation|tx-tcp6-segmentation|tx-tcp-ecn'
sysctl net.ipv4.tcp_ecn
"""

SNAP = (
    "echo '#ETHTOOL'; ethtool -S rocm-ernic0; echo '#NSTAT'; nstat -az | tail -n +2\n"
)


def g(n, script, timeout=900, check=False):
    """Run @script in guest @n with bash; with @check, raise if it fails."""
    r = subprocess.run(
        [GSSH, str(n), "-"],
        input=script,
        text=True,
        capture_output=True,
        timeout=timeout,
        check=False,
    )
    if check and r.returncode:
        raise RuntimeError(f"guest {n}: rc {r.returncode}\n{r.stdout}\n{r.stderr}")
    return r


def guest_counters(n):
    """Guest @n's ethtool statistics and nstat counters, as two dicts."""
    out = g(n, SNAP, check=True).stdout
    eth, ns, sec = {}, {}, None
    for line in out.splitlines():
        if line.startswith("#"):
            sec = line
            continue
        if sec == "#ETHTOOL" and ":" in line:
            k, v = line.rsplit(":", 1)
            try:
                eth[k.strip()] = int(v)
            except ValueError:
                pass
        elif sec == "#NSTAT":
            f = line.split()
            if len(f) >= 2:
                ns[f[0]] = int(f[1])
    return eth, ns


def emu_stats_raw(n):
    """Emulator @n's stats file as it is now, as a dict."""
    r = subprocess.run(
        f"docker cp ionic-devmem-ernic-{n}-1:/run/ernic-stats/ernic-{n}.stats - | tar -xO",
        shell=True,
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    d = {}
    for line in r.stdout.splitlines():
        m = re.match(r"\s*([A-Za-z_0-9 ]+?)\s*:\s*(\d+)\s*$", line)
        if m and m.group(1) not in d:  # first section wins (LIF before port)
            d[m.group(1).strip()] = int(m.group(2))
    return d


def emu_stats(n):
    """Emulator stats after at least one full rewrite of the stats file
    (it is rewritten every second): wait for the write count to move twice."""
    first = emu_stats_raw(n)["Write count"]
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        d = emu_stats_raw(n)
        if d.get("Write count", 0) >= first + 2:
            return d
        time.sleep(0.3)
    raise RuntimeError(f"ernic-{n} stats file is not advancing")


def snapshot():
    """The counters of both guests and both emulators."""
    s = {}
    for n in (1, 2):
        eth, ns = guest_counters(n)
        s[n] = {"eth": eth, "nstat": ns, "emu": emu_stats(n)}
    return s


def delta(a, b, keys=None):
    """The counters, of @keys if given, that changed from @a to @b, and by how much."""
    return {
        k: b[k] - a.get(k, 0)
        for k in b
        if (keys is None or k in keys) and b[k] != a.get(k, 0)
    }


def sum_q(d, suffix):
    """The sum over the Tx queues of the per-queue counter tx_<q>_@suffix."""
    return sum(v for k, v in d.items() if re.fullmatch(rf"tx_\d+_{suffix}", k))


# ---------------------------------------------------------------- capture

CAP_START = r"""
sudo rm -f /tmp/cap.pcap /tmp/cap.err
sudo ethtool -K rocm-ernic0 gro off
sudo nohup tcpdump -Z root -i rocm-ernic0 -B 131072 -s 0 -w /tmp/cap.pcap 'ether src @MAC@' >/tmp/cap.err 2>&1 &
for i in $(seq 100); do grep -q 'listening on' /tmp/cap.err 2>/dev/null && exit 0; sleep 0.1; done
echo "tcpdump did not start" >&2; cat /tmp/cap.err >&2; exit 1
"""
CAP_STOP = r"""
sudo pkill -INT -x tcpdump
for i in $(seq 100); do pgrep -x tcpdump >/dev/null || break; sleep 0.1; done
sudo ethtool -K rocm-ernic0 gro on
cat /tmp/cap.err >&2
sudo tcpdump -nn -vv -e -S -r /tmp/cap.pcap 2>/dev/null | gzip -1
sudo rm -f /tmp/cap.pcap
"""

BAD = re.compile(
    r"bad cksum|incorrect|bad udp cksum|bad icmp6? cksum|bad-len|truncated"
)
FLOW = re.compile(r" (\S+) > (\S+): Flags \[([^\]]*)\]")
SEQ = re.compile(r" seq (\d+)(?::(\d+))?")


# One pass over a decoded capture, with every check on a frame in one place.
# pylint: disable-next=too-many-locals,too-many-branches,too-many-statements
def analyze_capture(text, max_frame):
    """Check every frame of the tcpdump text @text; returns the findings as a dict."""
    recs, cur = [], None
    for line in text.splitlines():
        if line[:1] in (" ", "\t") and cur is not None:
            cur += " " + line.strip()
        else:
            if cur is not None:
                recs.append(cur)
            cur = line
    if cur is not None:
        recs.append(cur)
    res = {
        "frames": len(recs),
        "bad_cksum": 0,
        "udp_no_cksum": 0,
        "max_frame": 0,
        "over_mtu_frames": 0,
        "tcp_data_segs": 0,
        "max_tcp_payload": 0,
        "seq_gaps": 0,
        "seq_rexmit": 0,
        "fin_dup": 0,
        "ipid_nonseq": 0,
        "ipid_checked": 0,
        "ipid_zero": 0,
        "ipid_not_zero_or_next": 0,
        "cwr_data_segs": 0,
        "psh_segs": 0,
        "vlan_frames": 0,
        "bad_examples": [],
    }
    nxt, last_id, prev_id, fins = {}, {}, {}, set()
    for r in recs:
        m = re.search(r"length (\d+):", r)
        flen = int(m.group(1)) if m else 0
        res["max_frame"] = max(res["max_frame"], flen)
        if flen > max_frame:
            res["over_mtu_frames"] += 1
        if " vlan " in r:
            res["vlan_frames"] += 1
        if BAD.search(r):
            res["bad_cksum"] += 1
            if len(res["bad_examples"]) < 5:
                res["bad_examples"].append(r[:400])
        if "[no cksum]" in r:
            res["udp_no_cksum"] += 1
        f = FLOW.search(r)
        if not f:
            continue
        flow = (f.group(1), f.group(2))
        flags = f.group(3)
        mid = re.search(r"[(,] id (\d+),", r)
        if mid:
            ipid = int(mid.group(1))
            if flow in last_id:
                res["ipid_checked"] += 1
                if (ipid - last_id[flow]) & 0xFFFF != 1:
                    res["ipid_nonseq"] += 1
            last_id[flow] = ipid
        # v7.4 (RFC 6864) gives every DF packet ID 0, TSO templates
        # included, and a device without MANGLEID then numbers a chain's
        # segments 0, 1, 2, ... as software GSO does: there, an ID is
        # right if it is 0 or the previous one plus one.
        if mid:
            if ipid == 0:
                res["ipid_zero"] += 1
            elif flow in prev_id and (ipid - prev_id[flow]) & 0xFFFF != 1:
                res["ipid_not_zero_or_next"] += 1
            prev_id[flow] = ipid
        s = SEQ.search(r)
        if not s:
            continue
        if s.group(2) is None:  # SYN/FIN without data
            if "S" in flags:
                nxt[flow] = int(s.group(1)) + 1
            if "F" in flags:
                # A FIN seen twice: the first copy had arrived, so the
                # second is a retransmission nothing needed.
                key = (flow, s.group(1))
                if key in fins:
                    res["fin_dup"] += 1
                fins.add(key)
            continue
        a, b = int(s.group(1)), int(s.group(2))
        res["tcp_data_segs"] += 1
        res["max_tcp_payload"] = max(res["max_tcp_payload"], b - a)
        if "W" in flags:
            res["cwr_data_segs"] += 1
        if "P" in flags:
            res["psh_segs"] += 1
        exp = nxt.get(flow)
        if exp is not None and a != exp:
            if (a - exp) & 0xFFFFFFFF < 0x80000000:
                res["seq_gaps"] += 1
            else:
                res["seq_rexmit"] += 1
            if len(res["bad_examples"]) < 5:
                res["bad_examples"].append(f"seq expected {exp}: {r[:300]}")
        nxt[flow] = b
    return res


# ------------------------------------------------------------------ suite


def tx_script(dst, mtu, fam):
    """The traffic one direction carries while the receiver captures."""
    ip_hdr = FAMILIES[fam]["ip_hdr"]
    mss = mtu - ip_hdr - 20 - 12  # with timestamps
    udp_max = mtu - ip_hdr - 8  # largest unfragmented datagram
    sizes = [
        1,
        2,
        100,
        mss - 1,
        mss,
        mss + 1,
        2 * mss,
        2 * mss + 1,
        44 * mss,
        45 * mss + 1,
        65535,
        65536,
        65537,
        1048577,
        16777216,
    ]
    tx = f"python3 {GT} tx {dst} {PORT}"
    udp_all = 65507 if ip_hdr == 20 else 65527
    cmds = [
        f"{tx} {' '.join(map(str, sizes))}",
        f"{tx} 300000 --mode small",
        f"{tx} 300000 --mode small --nodelay",
        f"{tx} 6000 --mode small --nodelay --pattern 1",
        f"{tx} 200000 --mode small --pattern 1,2,3",
        f"{tx} 1000001 --mss 536",
        f"{tx} 100001 --mss 88",
        f"{tx} 4194304 --mode sendfile",
        f"{tx} 4194305 --mode sendfile --filedir /var/tmp",
        f"{tx} 500000 --mode sendfile --chunk 97 --cork",
        f"{tx} 300000 --mode sendfile --chunk 64 --cork",
        f"{tx} 600000 --mode sendfile --chunk 300 --cork",
        f"{tx} 2000000 --mode sendfile --chunk 5000 --cork",
        f"{tx} 4000000 --mode zerocopy",
        f"{tx} 500000 --mode zerocopy --chunk 5000",
        f"{tx} 3000000 --mode zerocopy --chunk 1000000",
        f"python3 {GT} udp {dst} {PORT} 1 2 17 18 19 64 512 1024 "
        f"{udp_max - 1} {udp_max} {udp_max + 1} 4000 9000 20000 {udp_all} --repeat 3",
        f"python3 {GT} udp {dst} {PORT} 8 64 1000 {udp_max - (udp_max & 1)} --zero-csum",
    ]
    if ip_hdr == 40:
        # TSO6 and checksum offload behind IPv6 extension headers.
        m8, m16 = mss - 8, mss - 16
        cmds += [
            f"sudo {tx} 1 {m8} {m8 + 1} {44 * m8} 1048577 4194304 --v6opts dst",
            f"sudo {tx} 1 {m16} {m16 + 1} 65536 1048577 4194304 --v6opts both",
            f"sudo {tx} 300000 --mode sendfile --chunk 97 --cork --v6opts hop",
        ]
    return "\n".join(cmds) + "\n", mss


def ping_script(dst, mtu, fam):
    """The guest script that pings @dst with sizes around the MTU and above it."""
    ip_hdr = FAMILIES[fam]["ip_hdr"]
    edge = mtu - ip_hdr - 8
    top = 65507 if ip_hdr == 20 else 65527
    sizes = [
        0,
        1,
        17,
        18,
        19,
        56,
        edge - 3,
        edge - 2,
        edge - 1,
        edge,
        edge + 1,
        edge + 2,
        edge + 3,
        2 * edge,
        2 * edge + 1,
        8000,
        20000,
        top,
    ]
    return (
        f"fail=0; for s in {' '.join(map(str, sizes))}; do "
        f"out=$(sudo ping -q -c 3 -i 0.02 -W 2 -s $s {dst} 2>&1 |"
        " grep -E 'packets transmitted|rror'); "
        "case \"$out\" in *' 0% packet loss'*) ;; "
        '*) fail=1; echo "PINGFAIL size $s: $out";; esac; done; '
        f"sudo ping -q -M do -c 2 -i 0.02 -W 2 -s {edge} {dst} |"
        " grep -q ' 0% packet loss' || { fail=1; echo 'PINGFAIL DF at MTU'; }; "
        "echo PINGDONE fail=$fail\n"
    )


def json_lines(text):
    """The JSON objects on the lines of @text that hold one."""
    out = []
    for line in text.splitlines():
        if line.startswith("{"):
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out


# pylint: disable-next=too-many-arguments,too-many-positional-arguments,too-many-locals,too-many-branches,too-many-statements
def run_direction(src, dst, cfg, fam, evdir, tag, bulk_sizes):
    """One direction of a suite, in the order it runs: capture, send, ping, check.

    (The pylint limits are off for it: the steps share their state, and splitting
    them would hide that order.)
    """
    mtu = CONFIGS[cfg][0]
    addr = FAMILIES[fam]["addr"](dst)
    res = {"dir": f"{src}->{dst}", "fail": [], "retrans": []}
    rxlog0 = int(g(dst, "wc -l < /tmp/gt-rx.log\n", check=True).stdout)

    g(dst, CAP_START.replace("@MAC@", MAC[src]), check=True)
    script, mss = tx_script(addr, mtu, fam)
    r = g(src, script + ping_script(addr, mtu, fam), timeout=1800)
    cap = subprocess.run(
        [GSSH, str(dst), "-"],
        input=CAP_STOP.encode(),
        capture_output=True,
        timeout=600,
        check=False,
    )
    capfile = evdir / f"{tag}-{src}to{dst}-capture.txt.gz"
    capfile.write_bytes(cap.stdout)
    cap_err = cap.stderr.decode(errors="replace")
    m = re.search(r"(\d+) packets dropped by kernel", cap_err)
    cap_dropped = int(m.group(1)) if m else -1
    text = gzip.decompress(cap.stdout).decode(errors="replace") if cap.stdout else ""
    ca = analyze_capture(text, mtu + 14 + FAMILIES[fam]["vlan"])
    ca["dropped_by_kernel"] = cap_dropped
    ca["mss"] = mss
    res["capture"] = ca
    res["capture_file"] = str(capfile)

    lines = json_lines(r.stdout)
    res["stderr"] = r.stderr[-2000:]
    res["ping"] = [x for x in r.stdout.splitlines() if x.startswith("PING")]

    for x in lines:
        x["captured"] = True
    rb = g(
        src,
        f"python3 {GT} tx {addr} {PORT} {' '.join(map(str, bulk_sizes))}\n",
        timeout=1800,
    )
    lines += json_lines(rb.stdout)
    res["stderr"] += rb.stderr[-2000:]
    res["tx"] = lines

    tcp = [x for x in lines if x.get("mode") != "udp"]
    # The receiver logs a connection once its reply is acknowledged: wait
    # for the lines, with a deadline.
    rxl = g(
        dst,
        f"for i in $(seq 100); do [ $(wc -l < /tmp/gt-rx.log) -ge {rxlog0 + len(tcp)} ]"
        " && break; sleep 0.1; done; "
        f"tail -n +{rxlog0 + 1} /tmp/gt-rx.log\n",
        check=True,
    ).stdout
    rx = json_lines(rxl)
    res["rx"] = rx
    udp = [x for x in lines if x.get("mode") == "udp"]
    res["rx_log_lines"] = len(rx)

    expect_tcp = (
        15 + 15 + len(bulk_sizes) + (13 if FAMILIES[fam]["ip_hdr"] == 40 else 0)
    )
    if len(tcp) != expect_tcp:
        res["fail"].append(f"{len(tcp)} TCP transfers reported, expected {expect_tcp}")
    if len(udp) != 2:
        res["fail"].append(f"{len(udp)} UDP runs reported, expected 2")
    for x in tcp:
        if not x.get("match"):
            res["fail"].append(f"hash mismatch {x['mode']} {x['bytes']}")
        if x.get("total_retrans"):
            res["retrans"].append(
                f"{x['total_retrans']} (dsack {x.get('dsack_dups')}) in {x['mode']} {x['bytes']}"
            )
            # Not captured (bulk phase): every retransmit must have been
            # DSACKed. Captured: judged against the capture, below.
            if not x.get("captured") and x["total_retrans"] != x.get("dsack_dups"):
                res["fail"].append(
                    f"{x['mode']} {x['bytes']}: {x['total_retrans']} retransmits, only "
                    f"{x.get('dsack_dups')} reported as duplicates by the receiver"
                )
    # Captured phase: the receiver's capture must show every retransmitted
    # segment twice (data or FIN) with no gap, i.e. the original arrived and
    # the retransmission was not needed for delivery.
    cap_retr = sum(x.get("total_retrans", 0) for x in tcp if x.get("captured"))
    if cap_retr > ca["seq_rexmit"] + ca["fin_dup"]:
        res["fail"].append(
            f"{cap_retr} retransmits by the sender, but the receiver's capture shows only "
            f"{ca['seq_rexmit']} duplicate data segments and {ca['fin_dup']} duplicate FINs"
        )
    res["rx_retrans"] = []
    for x in rx:
        if x.get("unacked"):
            res["fail"].append(
                f"receiver reply for {x.get('bytes')} never acknowledged"
            )
        if x.get("total_retrans"):
            res["rx_retrans"].append(
                f"{x['total_retrans']} (dsack {x.get('dsack_dups')})"
                f" on the reply for {x.get('bytes')}"
            )
            if x["total_retrans"] != x.get("dsack_dups"):
                res["fail"].append(
                    f"reply for {x.get('bytes')}: {x['total_retrans']} "
                    f"retransmits, {x.get('dsack_dups')} duplicates"
                )
    if [(x["bytes"], x["sha"]) for x in tcp] != [
        (x.get("bytes"), x.get("sha")) for x in rx
    ]:
        res["fail"].append("receiver log differs from what the sender sent")
    for x in udp:
        if not x.get("match"):
            res["fail"].append(
                f"udp: {x['good']}/{x['sent']} good, "
                f"{x['lost']} lost, {x['bad']} bad"
            )
    if "PINGDONE fail=0" not in res["ping"]:
        res["fail"].append("ping sweep: " + "; ".join(res["ping"]))
    if r.stderr.strip() or rb.stderr.strip():
        res["fail"].append("sender stderr not empty")
    # Which IP ID rule applies is read off the capture: a kernel that numbers
    # every packet from the socket's counter (v7.2) sends ID 0 only on wrap.
    if ca["ipid_zero"] * 100 > ca["ipid_checked"]:
        ca["ipid_rule"], ca["ipid_bad"] = "0-or-next", ca["ipid_not_zero_or_next"]
    else:
        ca["ipid_rule"], ca["ipid_bad"] = "next", ca["ipid_nonseq"]
    for k in (
        "bad_cksum",
        "over_mtu_frames",
        "seq_gaps",
        "dropped_by_kernel",
        "ipid_bad",
    ):
        if ca[k]:
            res["fail"].append(f"capture {k}={ca[k]}")
    if ca["udp_no_cksum"]:
        res["fail"].append(
            f"capture: {ca['udp_no_cksum']} UDP datagrams without checksum"
        )
    if ca["tcp_data_segs"] == 0:
        res["fail"].append("capture saw no TCP data")
    if ca["max_tcp_payload"] > mss:
        res["fail"].append(f"TCP payload {ca['max_tcp_payload']} > mss {mss}")
    big = [x for x in lines if x.get("bytes", 0) >= 1 << 30]
    res["MiB_s_1GiB"] = big[0]["MiB_s"] if big else None
    return res


EMU_ERR = (
    "tx_desc_data_error",
    "tx_dma_error",
    "tx_desc_fetch_error",
    "tx_queue_error",
    "tx_queue_disabled",
    "rx_dma_error",
    "rx_desc_fetch_error",
    "rx_desc_data_error",
    "rx_queue_error",
    "frames_tx_bad",
    "frames_tx_truncated",
    "tx_ucast_drop_packets",
)
ETH_ERR = (
    "hw_tx_dropped",
    "hw_tx_aborted_errors",
    "rx_csum_error",
    "hw_rx_dropped",
    "hw_rx_over_errors",
    "hw_rx_missed_errors",
)
NSTAT_TLP = (
    "TcpExtTCPLossProbes",
    "TcpExtTCPDSACKRecv",
    "TcpExtTCPFastRetrans",
    "TcpExtTCPSlowStartRetrans",
    "TcpExtTCPLossProbeRecovery",
)
NSTAT_ERR = (
    "TcpInCsumErrors",
    "UdpInCsumErrors",
    "Udp6InCsumErrors",
    "IpInHdrErrors",
    "Ip6InHdrErrors",
    "TcpRetransSegs",
    "IcmpInCsumErrors",
    "Icmp6InCsumErrors",
    "TcpInErrs",
    "IpReasmFails",
    "Ip6ReasmFails",
    "UdpInErrors",
    "Udp6InErrors",
    "TcpExtTCPTimeouts",
    "TcpExtTCPLostRetransmit",
)


# One suite, in the order it runs, like run_direction().
# pylint: disable-next=too-many-locals,too-many-branches
def run_suite(label, cfg, fam, bulk):
    """One configuration and address family, both directions; returns its result."""
    mtu, extra, expect = CONFIGS[cfg]
    evdir = EV / label
    evdir.mkdir(parents=True, exist_ok=True)
    tag = f"{cfg}-{fam}"
    out = {
        "label": label,
        "config": cfg,
        "family": fam,
        "mtu": mtu,
        "started": time.strftime("%F %T"),
        "setup": {},
        "fail": [],
        "notes": [],
    }
    for n in (1, 2):
        r = g(
            n,
            RESET.replace("@MTU@", str(mtu))
            .replace("@N@", str(n))
            .replace("@EXTRA@", extra)
            .replace("@VLAN@", "1" if FAMILIES[fam]["vlan"] else "0"),
            check=True,
        )
        out["setup"][n] = (r.stdout + r.stderr).strip().splitlines()
    # Neighbour resolution before counting, so no transfer starts on ARP or
    # neighbour discovery.
    a2 = FAMILIES[fam]["addr"](2)
    r = g(
        1,
        f"for i in $(seq 50); do ping -q -c 1 -W 1 {a2} >/dev/null 2>&1 && exit 0;"
        " sleep 0.2; done; exit 1\n",
    )
    if r.returncode:
        out["fail"].append("peer not reachable after setup")
    before = snapshot()
    out["dirs"] = [
        run_direction(1, 2, cfg, fam, evdir, tag, bulk),
        run_direction(2, 1, cfg, fam, evdir, tag, bulk),
    ]
    after = snapshot()
    out["delta"] = {}
    for n in (1, 2):
        de = delta(before[n]["eth"], after[n]["eth"])
        dn = delta(before[n]["nstat"], after[n]["nstat"], NSTAT_ERR)
        dt = delta(before[n]["nstat"], after[n]["nstat"], NSTAT_TLP)
        # Retransmits. Authoritative: TCP_INFO of every test socket (the
        # senders', and the receivers' for their one-line replies), checked
        # in run_direction: each retransmitted segment must be one the peer
        # reported as a duplicate (DSACK), i.e. the original transmission
        # had delivered it and the retransmit was a spurious probe (notes:
        # a ~10 ms stall every 5.12 s that the pre-change server has too).
        # The guest-wide counter also sees the ssh sessions driving the
        # test; what the test sockets do not account for is noted.
        retr = dn.pop("TcpRetransSegs", 0)
        mine = out["dirs"][n - 1]["tx"]
        theirs = out["dirs"][2 - n]["rx"]
        test_retr = sum(x.get("total_retrans", 0) for x in mine + theirs)
        if test_retr:
            out["notes"].append(
                f"devmem-{n}: {test_retr} retransmit(s) on test sockets (spurious, i.e. the "
                "original was seen delivered, unless a failure above says otherwise): "
                + "; ".join(
                    out["dirs"][n - 1]["retrans"] + out["dirs"][2 - n]["rx_retrans"]
                )
            )
        if retr != test_retr:
            out["notes"].append(
                f"devmem-{n}: guest-wide TcpRetransSegs +{retr}, "
                f"test sockets {test_retr} (rest: non-test sockets, e.g. ssh)"
            )
        dm = delta(before[n]["emu"], after[n]["emu"])
        dm.pop("Write count", None)
        summ = {
            "tx_tso": de.get("tx_tso", 0),
            "tx_csum": de.get("tx_csum", 0),
            "tx_csum_none": de.get("tx_csum_none", 0),
            "tx_frags": sum_q(de, "frags"),
            "tx_linearize": sum_q(de, "linearize"),
            "tx_dma_map_err": sum_q(de, "dma_map_err"),
            "tx_packets": de.get("tx_packets", 0),
        }
        out["delta"][n] = {
            "guest_summary": summ,
            "nstat_errs": dn,
            "nstat_tlp": dt,
            "emu": {
                k: v for k, v in dm.items() if k in EMU_ERR or k.startswith("tx_ucast")
            },
            "eth_errs": {k: v for k, v in de.items() if k in ETH_ERR},
        }
        for k in EMU_ERR:
            if dm.get(k):
                out["fail"].append(f"emulator ernic-{n} {k} +{dm[k]}")
        for k in ETH_ERR:
            if de.get(k):
                out["fail"].append(f"devmem-{n} ethtool {k} +{de[k]}")
        for k, v in dn.items():
            out["fail"].append(f"devmem-{n} nstat {k} +{v}")
        if summ["tx_dma_map_err"]:
            out["fail"].append(f"devmem-{n} tx dma_map_err")
        if expect["tso"] != (summ["tx_tso"] > 0):
            out["fail"].append(
                f"devmem-{n} tx_tso delta {summ['tx_tso']}, "
                f"expected {'>0' if expect['tso'] else '0'}"
            )
        if expect["csum"] != (summ["tx_csum"] > 0):
            out["fail"].append(
                f"devmem-{n} tx_csum delta {summ['tx_csum']}, "
                f"expected {'>0' if expect['csum'] else '0'}"
            )
    for d in out["dirs"]:
        out["fail"] += [f"{d['dir']}: {x}" for x in d["fail"]]
    out["verdict"] = "FAIL" if out["fail"] else "PASS"
    (evdir / f"{tag}.json").write_text(json.dumps(out, indent=1))
    caps = [d["capture"] for d in out["dirs"]]
    line = "\t".join(
        str(x)
        for x in (
            out["started"],
            cfg,
            fam,
            out["verdict"],
            "/".join(str(d["MiB_s_1GiB"]) for d in out["dirs"]),
            "/".join(str(c["frames"]) for c in caps),
            "/".join(str(c["max_frame"]) for c in caps),
            "/".join(str(c["bad_cksum"]) for c in caps),
            "/".join(f"{c['ipid_bad']}({c['ipid_rule']})" for c in caps),
            "/".join(str(out["delta"][n]["guest_summary"]["tx_tso"]) for n in (1, 2)),
            "/".join(
                str(out["delta"][n]["guest_summary"]["tx_linearize"]) for n in (1, 2)
            ),
            "; ".join(out["fail"] + out["notes"])[:700],
        )
    )
    with open(evdir / "results.tsv", "a", encoding="ascii") as f:
        f.write(line + "\n")
    print(line, flush=True)
    return out["verdict"] == "PASS"


def main():
    """Run the suites the command line names, or all of them."""
    label = sys.argv[1]
    plan = sys.argv[2:] or DEFAULT_PLAN
    for n in (1, 2):
        g(
            n,
            "mountpoint -q /mnt/hostfs || sudo mount /mnt/hostfs; "
            "bash /mnt/hostfs/test/scripts/emu-offloads/rx-start.sh\n",
            check=True,
        )
    ok = True
    for item in plan:
        cfg, _, fam = item.partition(":")
        for f in [fam] if fam else ["v4", "v6"]:
            # 1 GiB on the untagged families; 64 MiB everywhere.
            bulk = (
                [67108867, 1073741824] if f in ("v4", "v6") else [67108867, 268435456]
            )
            ok &= run_suite(label, cfg, f, bulk)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
