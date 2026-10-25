#!/bin/sh
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Print the index of the IPv4-mapped GID (::ffff:a.b.c.d) for ADDRESS on
# port 1 of RDMA device DEVICE in this guest.  If the port has no such
# GID, print its GID table to stderr and exit 1.
#
# The RDMA tests address a peer by its IPv4-mapped GID, and its index is
# not fixed: the kernel adds a GID for each address on the netdev as the
# address appears.  On our guests it is usually at index 1, but two
# guests booted from one image have disagreed, one with its IPv6
# link-local GID at 1.  A fixed index then picks an IPv6 GID on one side
# and an IPv4 one on the other, and the QP never reaches RTR.
#
# Intended to be run in the guest:
#   ssh guest 'sh -s -- DEVICE ADDRESS' < scripts/find-ipv4-gid-index.sh
# Deliberately POSIX sh and free of external commands, like
# find-rdma-device.sh, so it works on a minimal guest image.

SYSFS_ROOT=${ERNIC_SYSFS_ROOT:-/sys}

usage() {
    echo "usage: find-ipv4-gid-index.sh DEVICE IPV4-ADDRESS" >&2
    exit 2
}

bad_address() {
    echo "find-ipv4-gid-index.sh: not a dotted-quad IPv4 address: $addr" >&2
    exit 2
}

[ $# -eq 2 ] || usage
dev=$1
addr=$2

# The name becomes a path component under class/infiniband.
case $dev in
    '' | . | .. | */*) usage ;;
esac

# read leaves a trailing dot out of the last field, so catch it here.
case $addr in
    *.) bad_address ;;
esac
IFS=. read -r o1 o2 o3 o4 extra <<EOF
$addr
EOF
[ -z "$extra" ] || bad_address
for octet in "$o1" "$o2" "$o3" "$o4"; do
    # No leading zeros: printf would read 010 as octal.
    case $octet in
        '' | *[!0-9]* | 0?*) bad_address ;;
    esac
    if [ ${#octet} -gt 3 ] || [ "$octet" -gt 255 ]; then
        bad_address
    fi
done

# The form sysfs prints a GID in.
want=$(printf '0000:0000:0000:0000:0000:ffff:%02x%02x:%02x%02x' \
    "$o1" "$o2" "$o3" "$o4")
zero=0000:0000:0000:0000:0000:0000:0000:0000

gids_dir=$SYSFS_ROOT/class/infiniband/$dev/ports/1/gids
if [ ! -d "$gids_dir" ]; then
    echo "find-ipv4-gid-index.sh: no GID table at $gids_dir" >&2
    exit 1
fi

# By index rather than by glob, which would sort 10 before 2, so a
# GID listed twice resolves to its lowest index.
i=0
while [ -r "$gids_dir/$i" ]; do
    if read -r gid < "$gids_dir/$i" && [ "$gid" = "$want" ]; then
        echo "$i"
        exit 0
    fi
    i=$((i + 1))
done

echo "find-ipv4-gid-index.sh: $dev has no GID for $addr ($want)" >&2
echo "port 1 GIDs in use:" >&2
i=0
while [ -r "$gids_dir/$i" ]; do
    if read -r gid < "$gids_dir/$i" && [ "$gid" != "$zero" ]; then
        echo "  $i: $gid" >&2
    fi
    i=$((i + 1))
done
exit 1
