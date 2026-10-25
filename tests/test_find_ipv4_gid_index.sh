#!/usr/bin/env bash
#
# scripts/find-ipv4-gid-index.sh against synthetic sysfs GID tables: it
# must find the IPv4-mapped GID wherever it sits, report the lowest index
# when it is listed twice, fail with the table when it is missing, and
# reject malformed arguments.  The script runs under sh, as in a guest.
#
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Exit 0 = pass, exit 1 = fail.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
LOOKUP_SCRIPT="$SCRIPT_DIR/../scripts/find-ipv4-gid-index.sh"
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/test-find-ipv4-gid-index-XXXXXX")
ERNIC_SYSFS_ROOT="$WORK_DIR/sys"
export ERNIC_SYSFS_ROOT
trap 'rm -rf "$WORK_DIR"' EXIT HUP INT TERM

DEV=rdma0
GIDS_DIR="$ERNIC_SYSFS_ROOT/class/infiniband/$DEV/ports/1/gids"
ZERO=0000:0000:0000:0000:0000:0000:0000:0000
LINK_LOCAL=fe80:0000:0000:0000:00a0:d1ff:fe00:0001
# 192.168.200.10 and 192.168.200.20
V4_10=0000:0000:0000:0000:0000:ffff:c0a8:c80a
V4_20=0000:0000:0000:0000:0000:ffff:c0a8:c814

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

# set_gids GID...: a 16-entry table holding the GIDs from index 0, and
# zeros after them, as sysfs shows unused entries.
set_gids() {
    rm -rf "$GIDS_DIR"
    mkdir -p "$GIDS_DIR"
    local i=0 gid
    for gid in "$@"; do
        printf '%s\n' "$gid" > "$GIDS_DIR/$i"
        i=$((i + 1))
    done
    while [ "$i" -lt 16 ]; do
        printf '%s\n' "$ZERO" > "$GIDS_DIR/$i"
        i=$((i + 1))
    done
}

# expect_index INDEX DEVICE ADDRESS
expect_index() {
    local want=$1 out
    shift
    out=$(sh "$LOOKUP_SCRIPT" "$@") ||
        fail "lookup of $* failed; expected index $want"
    [ "$out" = "$want" ] || fail "lookup of $*: expected $want, got '$out'"
}

# expect_status STATUS DEVICE ADDRESS: fails with STATUS and no stdout.
# Leaves stderr in $WORK_DIR/err.
expect_status() {
    local want=$1 out rc=0
    shift
    out=$(sh "$LOOKUP_SCRIPT" "$@" 2> "$WORK_DIR/err") || rc=$?
    [ "$rc" -eq "$want" ] ||
        fail "lookup of '$*': expected exit $want, got $rc"
    [ -z "$out" ] || fail "lookup of '$*' printed '$out' on stdout"
}

# The usual layout: default GID, then the IPv4-mapped one.
set_gids "$LINK_LOCAL" "$V4_10"
expect_index 1 "$DEV" 192.168.200.10

# The layout that failed the TCP mesh lane: the link-local twice.
set_gids "$LINK_LOCAL" "$LINK_LOCAL" "$V4_10"
expect_index 2 "$DEV" 192.168.200.10

# The address picks its own GID, not just any IPv4-mapped one.
set_gids "$LINK_LOCAL" "$V4_10" "$V4_20"
expect_index 2 "$DEV" 192.168.200.20

# Octets below 16 are zero-padded.
set_gids "$LINK_LOCAL" 0000:0000:0000:0000:0000:ffff:0a00:0001
expect_index 1 "$DEV" 10.0.0.1

# Listed twice, at 2 and 10: the lowest index wins, though a glob would
# reach 10 first.
set_gids "$LINK_LOCAL" "$V4_20" "$V4_10" "$V4_20" "$V4_20" "$V4_20" \
    "$V4_20" "$V4_20" "$V4_20" "$V4_20" "$V4_10"
expect_index 2 "$DEV" 192.168.200.10

# Missing: exit 1, naming the address and listing only the entries in use.
set_gids "$LINK_LOCAL" "$V4_20"
expect_status 1 "$DEV" 192.168.200.10
grep -q "$V4_10" "$WORK_DIR/err" || fail "missing GID not named on stderr"
grep -q "^  0: $LINK_LOCAL\$" "$WORK_DIR/err" ||
    fail "GID table not listed on stderr"
grep -q "^  1: $V4_20\$" "$WORK_DIR/err" ||
    fail "GID table not listed on stderr"
if grep -q "$ZERO" "$WORK_DIR/err"; then
    fail "unused GID entries listed on stderr"
fi

# No such device.
expect_status 1 rdma1 192.168.200.10

# Malformed arguments: exit 2.
set_gids "$LINK_LOCAL" "$V4_10"
for addr in '' 192.168.200 192.168.200.10. .192.168.200.10 \
    192.168.200.10.1 192..200.10 256.168.200.10 1000.168.200.10 \
    192.168.200.010 192.168.200.0x0a 192.168.200.-1 \
    '192.168.200.1 0' fe80::1; do
    expect_status 2 "$DEV" "$addr"
done
for dev in '' . .. ../rdma0 rdma0/ports; do
    expect_status 2 "$dev" 192.168.200.10
done
expect_status 2 "$DEV"
expect_status 2 "$DEV" 192.168.200.10 extra

echo "IPv4-mapped GID lookup finds the address's GID at any index"
