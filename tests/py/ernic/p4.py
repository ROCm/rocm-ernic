"""The P4 control surface: ionic device commands 70, 71 and 72.

Upstream's pds_fwctl opcodes with rocm-ernic's own wire format; see
docs/p4.rst, "Shaped like pds_fwctl, not pds_fwctl". A guest reaches the
match-action engine only through these, and only as far as the scope the
operator granted with --p4-guest-scope.

Its own module rather than more of ionic.py, which is already exactly at
pylint's ceiling for one file. These take the driver as their first
argument rather than being mixed into it, so reaching the P4 surface
costs ionic.py nothing at all.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import collections
import struct

from .errors import DeviceError
from .ionic import BAR0_DEVCMD_COMP

CMD_P4_IDENT = 70
CMD_P4_RPC = 71
CMD_P4_QUERY = 72

# enum p4_endpoint
P4_EP_PROGRAM = 1
P4_EP_TABLE = 2
P4_EP_STATS = 3

# Operations, per endpoint.
P4_OP_LOAD_BEGIN = 1
P4_OP_LOAD_CHUNK = 2
P4_OP_LOAD_COMMIT = 3
P4_OP_LOAD_ABORT = 4
P4_OP_ENTRY_UPDATE = 1
P4_OP_STATS_READ = 1

# enum p4_scope, as the caller's own claim.
P4_SCOPE_NONE = 0
P4_SCOPE_READ = 1
P4_SCOPE_WRITE = 2
P4_SCOPE_LOAD = 3

# enum p4_wire_update_type: the same values as the host's enum
# p4_update_type, which p4_devcmd.c asserts, but spelled separately
# because a guest cannot see the host header.
P4_UPDATE_INSERT = 0
P4_UPDATE_MODIFY = 1
P4_UPDATE_DELETE = 2

P4_DEVCMD_NAME_MAX = 63
P4_DEVCMD_VALUE_MAX = 16

# enum p4_fmt, src/p4/p4_fmt.h.  The numbering is ABI: a guest's program
# names these, so they are appended to and never renumbered.
P4_FMT_FP32 = 0
P4_FMT_FP16 = 1
P4_FMT_BF16 = 2
P4_FMT_FP8_E4M3 = 3
P4_FMT_FP8_E5M2 = 4
P4_FMT_FP4_E2M1 = 5

# enum p4_fp8_op, the arithmetic map() selects.
P4_FP8_OP_ADD = 0
P4_FP8_OP_MUL = 1
P4_FP8_OP_MAX = 2


def p4_entry_update(update_type, table, action, matches=(), params=()):
    """Build a P4_EP_TABLE / P4_OP_ENTRY_UPDATE request body.

    @matches and @params are sequences of bytes, each the significant
    value of one field, given most significant byte first as it would be
    written down.  They are right-aligned into the fixed-width slots the
    device expects, which is where every other value in the engine lives.
    """
    if len(table) > P4_DEVCMD_NAME_MAX or len(action) > P4_DEVCMD_NAME_MAX:
        raise ValueError("a table or action name is at most 63 bytes")

    # struct p4_rpc_entry_update: type, 3 rsvd, priority, nmatch, nparams,
    # then the two 64-byte names.
    body = struct.pack(
        "<B3xIHH",
        update_type,
        0,
        len(matches),
        len(params),
    )
    body += table.encode().ljust(P4_DEVCMD_NAME_MAX + 1, b"\0")
    body += action.encode().ljust(P4_DEVCMD_NAME_MAX + 1, b"\0")

    for value in matches:
        # struct p4_rpc_entry_match: len, prefix_bits, value[16], mask[16]
        body += struct.pack("<HH", len(value), 0)
        body += bytes(value).rjust(P4_DEVCMD_VALUE_MAX, b"\0")
        body += b"\0" * P4_DEVCMD_VALUE_MAX
    for value in params:
        # struct p4_rpc_entry_param: len, rsvd, value[16]
        body += struct.pack("<HH", len(value), 0)
        body += bytes(value).rjust(P4_DEVCMD_VALUE_MAX, b"\0")
    return body


def ident(drv):
    """IDENT the P4 surface, returning its struct p4_devcmd_ident.

    Returns a dict, or None when the device answers nothing -- which
    is what a server started without --p4-guest-scope does, and is a
    correct configuration rather than a failure.
    """
    buf = drv.mem.alloc(32, align=8)
    # struct p4_devcmd_ident_cmd: opcode, version, lif_index, len,
    # ident_pa, then padding to 64.
    cmd = struct.pack("<BBHIQ", CMD_P4_IDENT, 0, 0, 32, buf)
    cmd = cmd.ljust(64, b"\0")
    status, _ = drv.devcmd(cmd)
    if status != 0:
        return None
    raw = drv.mem.read(buf, 32)
    fields = struct.unpack("<8I", raw)
    return {
        "abi_version": fields[0],
        "scope": fields[1],
        "max_program": fields[2],
        "max_chunk": fields[3],
        "max_request": fields[4],
        "max_response": fields[5],
        "endpoints": fields[6],
    }


# An RPC names an endpoint, an operation on it, and the scope the caller
# claims -- "an endpoint id, an operation id, a scope the device checks",
# as docs/p4.rst puts it. One thing, so it travels as one.
Call = collections.namedtuple("Call", "ep op scope")

LOAD_BEGIN = Call(P4_EP_PROGRAM, P4_OP_LOAD_BEGIN, P4_SCOPE_LOAD)
LOAD_CHUNK = Call(P4_EP_PROGRAM, P4_OP_LOAD_CHUNK, P4_SCOPE_LOAD)
LOAD_COMMIT = Call(P4_EP_PROGRAM, P4_OP_LOAD_COMMIT, P4_SCOPE_LOAD)
ENTRY_UPDATE = Call(P4_EP_TABLE, P4_OP_ENTRY_UPDATE, P4_SCOPE_WRITE)
STATS_READ = Call(P4_EP_STATS, P4_OP_STATS_READ, P4_SCOPE_READ)


def rpc(drv, call, req=b"", resp_len=0):
    """One P4 RPC. Returns (status, response bytes).

    The request and the response get separate buffers because the
    device treats them as separate permissions.
    """
    req_pa = 0
    if req:
        req_pa = drv.mem.alloc(len(req), align=8)
        drv.mem.write(req_pa, req)
    resp_pa = 0
    if resp_len:
        resp_pa = drv.mem.alloc(resp_len, align=8)

    # struct p4_devcmd_rpc_cmd
    cmd = struct.pack(
        "<BBHIIIQIIQ",
        CMD_P4_RPC,
        call.scope,
        0,
        call.ep,
        call.op,
        len(req),
        req_pa,
        resp_len,
        0,
        resp_pa,
    )
    cmd = cmd.ljust(64, b"\0")
    status, _ = drv.devcmd(cmd)
    comp = drv.bar0_read(BAR0_DEVCMD_COMP, 16)
    got = struct.unpack_from("<I", comp, 4)[0]
    data = b""
    if resp_len and status == 0:
        data = drv.mem.read(resp_pa, min(got, resp_len))
    return status, data


def load_program(drv, blob, chunk=4096):
    """Push a compiled bmv2 program down, as a control plane would.

    Raises DeviceError naming the step that was refused.
    """
    status, _ = rpc(drv, LOAD_BEGIN, struct.pack("<II", len(blob), 0))
    if status != 0:
        raise DeviceError(f"P4 LOAD_BEGIN refused: status {status}")

    for off in range(0, len(blob), chunk):
        part = blob[off : off + chunk]
        # struct p4_rpc_load_chunk: a 64-bit offset, the length, a
        # reserved word, then the bytes -- so the device reads header
        # and payload with one DMA and a chunk cannot name memory its
        # own header did not.
        body = struct.pack("<QII", off, len(part), 0) + part
        status, _ = rpc(drv, LOAD_CHUNK, body)
        if status != 0:
            raise DeviceError(f"P4 LOAD_CHUNK at {off} refused: status {status}")

    status, _ = rpc(drv, LOAD_COMMIT)
    if status != 0:
        raise DeviceError(f"P4 LOAD_COMMIT refused: status {status}")


def entry(drv, update, matches=(), params=()):
    """Insert, modify or delete one table entry. Returns the status.

    @update is (update_type, table, action): which change, to which
    table's which action.
    """
    update_type, table, action = update
    status, _ = rpc(
        drv,
        ENTRY_UPDATE,
        p4_entry_update(update_type, table, action, matches, params),
    )
    return status


def stats(drv):
    """Read the engine's counters through the guest surface."""
    status, data = rpc(drv, STATS_READ, resp_len=56)
    if status != 0 or len(data) < 56:
        return None
    vals = struct.unpack("<7Q", data[:56])
    return {
        "host_in": vals[0],
        "uplink_in": vals[1],
        "host_out": vals[2],
        "uplink_out": vals[3],
        "dropped": vals[4],
        "too_big": vals[5],
        "parse_errors": vals[6],
    }
