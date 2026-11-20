#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# devmem-rx.sh start RCV [NCDEVMEM_ARGS...]   start `ncdevmem -l` on guest RCV (default args: first light)
# devmem-rx.sh wait RCV                       wait for ncdevmem to exit, print summary
# devmem-rx.sh summary RCV
S=$(dirname "$(readlink -f "$0")")
cmd=$1 n=$2; shift 2
ip=192.168.100.1$n
case "$cmd" in
start)
    args=${*:-"-n -l -s $ip -p 5201 -f rocm-ernic0 -t 0 -q 1 -v 7 -L"}
    "$S/gssh" "$n" "sudo pkill -x ncdevmem; sudo rm -f /tmp/ncdevmem.out /tmp/ncdevmem.err
nohup sudo sh -c 'ncdevmem $args > /tmp/ncdevmem.out 2> /tmp/ncdevmem.err; echo exit=\$? >> /tmp/ncdevmem.err' >/dev/null 2>&1 &
for i in \$(seq 150); do grep -q -E 'Waiting|exit=' /tmp/ncdevmem.err 2>/dev/null && break; sleep 0.2; done; head -n 6 /tmp/ncdevmem.err"
    ;;
wait)
    "$S/gssh" "$n" "for i in \$(seq ${1:-300}); do grep -q 'exit=' /tmp/ncdevmem.err && break; sleep 0.2; done"
    "$0" summary "$n"
    ;;
summary)
    # shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
    "$S/gssh" "$n" 'e=/tmp/ncdevmem.err
echo "ncdevmem: $(grep exit= $e || echo STILL-RUNNING); dmabuf cmsgs=$(grep -c "^received frag_page" $e) linear cmsgs=$(grep -c SCM_DEVMEM_LINEAR $e) recvmsg calls=$(grep -c "^recvmsg ret" $e)"
echo "  $(grep "^total_received" $e | tail -n 1) validated-buffers=$(grep -c "Validated buffer" /tmp/ncdevmem.out) validation-failures=$(grep -c "Failed validation" $e)"
grep -E "page_aligned_frags|ncdevmem: |Failed|error|EFAULT" $e | grep -v "^received" | sort | uniq -c | head -n 8'
    ;;
esac
