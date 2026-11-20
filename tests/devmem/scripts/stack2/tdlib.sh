#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Sourced by the devmem teardown cases. Receiver = guest 1, sender = guest 2.
S=${DEVMEM_ROOT:?set it to the work area}/test-rx/scripts
IP1=192.168.100.11 IP2=192.168.100.12
g() { "$S/gssh" "$@"; }
hdr() { echo; echo "=== $*"; }
PAT="yes \$(echo -e '\\x01\\x02\\x03\\x04\\x05\\x06')"
stream_bg() { # bytes [sport]: background pattern sender on guest 2
    g 2 "pkill socat; nohup sh -c \"$PAT | head -c $1 | socat -u - TCP:$IP1:5201${2:+,bind=$IP2:$2,reuseaddr}; echo sender-exit=\\\$? > /tmp/sender.rc\" >/dev/null 2>&1 & rm -f /tmp/sender.rc; echo sender started"
}
send_fg() { g 2 "$PAT | head -c $1 | timeout ${3:-60} socat -u - TCP:$IP1:5201${2:+,bind=$IP2:$2,reuseaddr}; echo sender rc=\$?"; }
wait_rx_bytes() { # bytes: wait until ncdevmem on guest 1 has received that much
    g 1 "for i in \$(seq 300); do n=\$(grep '^total_received' /tmp/ncdevmem.err 2>/dev/null | tail -n 1 | cut -d= -f2); [ \"\${n:-0}\" -ge $1 ] && { echo \"receiver at \$n bytes\"; exit 0; }; sleep 0.1; done; echo 'receiver never got $1 bytes'; exit 1"
}
pools() { echo "   pools rx-$1: $(g "$1" 'sudo /mnt/hostfs/test-rx/guest/pools.sh' | tr '\n' ' ')"; }
wait_no_dmabuf_pool() { # guest: wait for the dmabuf pool(s) to be released
    # shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
    g "$1" 'for i in $(seq 100); do sudo /mnt/hostfs/test-rx/guest/pools.sh | grep -q -E "dmabuf|orphans\(no ifindex\): [1-9]|detached: [1-9]" || { echo "   dmabuf/orphan pools gone after $i tries"; exit 0; }; sleep 0.2; done; echo "   !! dmabuf or orphan pool still present after 20 s"; sudo /mnt/hostfs/test-rx/guest/pools.sh'
}
health() { # after a case: splat, lockdep, pools, plain TCP both ways
    "$S/splat.sh"; for n in 1 2; do echo "   lockdep rx-$n: $(g $n "sudo grep debug_locks /proc/lockdep_stats | tr -s ' '")"; done
    pools 1
    g 1 "for i in \$(seq 50); do ping -c1 -W1 $IP2 >/dev/null 2>&1 && break; sleep 0.2; done; echo \"   ping tries \$i\""
    "$S/xfer.sh" 2 1 $IP1 $IP1 8000000 bulk; "$S/xfer.sh" 1 2 $IP2 $IP2 8000000 bulk
}
first_light() { # [bytes]: bind, send, validate, summary
    "$S/devmem-rx.sh" start 1 | tail -n 1; send_fg "${1:-4194304}"; "$S/devmem-rx.sh" wait 1 300
}
