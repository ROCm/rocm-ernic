#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# M6: unaligned offsets/lengths, many frags, the frag-limit negative case.
# shellcheck source=tests/devmem/scripts/stack1/devmem-tx/lib.sh
. "$(dirname "$(readlink -f "$0")")/lib.sh"
wait_dmabuf_gone() { # how long until the sender guest has no dma-buf object left
    # shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
    G1 't0=$(date +%s); for i in $(seq '"${1:-240}"'); do n=$(sudo awk "/^Total/{print \$2}" /sys/kernel/debug/dma_buf/bufinfo); [ "$n" = 0 ] && break; sleep 1; done; echo "dma-buf objects on sender: $n after $(( $(date +%s) - t0 ))s wait; orphan sockets: $(ss -tn state all dst '"$IP2"' | tail -n +2 | wc -l)"'
}
pos() {
    # -w 0: one send at a time (each awaits its completion), so every send is its own skb
    devmem_case M6a-1byte-odd-offsets 5420 "-S 4m -o 1 -l 1 -a 4097 -n 300" "-N -w 0"
    devmem_case M6b-2bytes-across-page-boundary 5421 "-S 4m -o 4095 -l 2 -a 4096 -n 300" "-N -w 0"
    devmem_case M6c-odd-len-odd-offset-one-iovec 5422 "-S 16m -o 123 -l 100003 -a 7919 -n 500"
    devmem_case M6d-50-iovecs-odd-sizes-gaps 5423 "-S 16m -o 3001 -l 250007 -z 5003 -g 1237 -a 104729 -n 300"
    devmem_case M6e-last-bytes-of-dmabuf 5424 "-S 4m -o 4190209 -l 4095 -n 50" "-N -w 0"
}
pos2() {
    devmem_case M6f-many-frags-128B-tso-on 5425 "-S 16m -o 7 -l 61440 -z 128 -g 64 -a 8191 -n 500"
    # 15 iovecs of 64 bytes, one per page, never crossing a page: a non-GSO skb with exactly 15 frags
    devmem_case M6g-15-frags-non-gso-exact-limit 5426 "-S 4m -o 9 -l 960 -z 64 -g 4032 -a 4096 -n 300" "-N -w 0"
    devmem_case M6h-1024-iovecs-of-1400B 5427 "-S 16m -o 77 -l 1433600 -z 1400 -g 2696 -a 4099 -n 40"
    local n; for n in 1 2; do "$S/gssh" $n "sudo ethtool -K $IF tso off"; done
    devmem_case M6i-many-frags-128B-tso-off 5428 "-S 16m -o 7 -l 61440 -z 128 -g 64 -a 8191 -n 500"
    for n in 1 2; do "$S/gssh" $n "sudo ethtool -K $IF tso on"; done
}
neg() {
    # 300 one-byte sends back to back: TCP appends them to the unsent skb until it has 16+ frags
    RXWAIT=40 devmem_case M6n0-NEG-back-to-back-1-byte-sends 5429 "-S 4m -o 1 -l 1 -a 4097 -n 300" "-N -t 10 -C 3" "-W 25"
    wait_dmabuf_gone 300
    # one frag more than M6g: a non-GSO skb with exactly 16 frags
    RXWAIT=40 devmem_case M6n4-NEG-16-frags-non-gso 5433 "-S 4m -o 9 -l 1024 -z 64 -g 4032 -a 4096 -n 300" "-N -w 0 -t 10 -C 3" "-W 25"
    wait_dmabuf_gone 300
    # every skb is 17 frags of 16 bytes: above the device's 15
    RXWAIT=40 devmem_case M6n1-NEG-17-frags-16B-sndtimeo 5430 "-S 4m -o 5 -l 16384 -z 16 -a 4099 -n 64" "-t 10 -C 3" "-W 25"
    wait_dmabuf_gone 300
    G1 "sudo sysctl -w net.ipv4.tcp_retries2=5"
    RXWAIT=60 devmem_case M6n2-NEG-17-frags-16B-blocking-retries2-5 5431 "-S 4m -o 5 -l 16384 -z 16 -a 4099 -n 64" "-C 20" "-W 45"
    wait_dmabuf_gone 120
    G1 "sudo sysctl -w net.ipv4.tcp_retries2=15"
    # the guest and the device still work: a positive run straight after
    devmem_case M6n3-positive-after-negative 5432 "-S 16m -o 123 -l 100003 -a 7919 -n 500"
}
edge() {
    # around the limit: 17 frags of N bytes; MSS 1448. N=80: 1360 < MSS, non-GSO with 17 frags.
    G1 "sudo sysctl -w net.ipv4.tcp_retries2=5"
    local z
    for z in 80 90 100 110; do
        RXWAIT=60 devmem_case "M6x-edge-frags-of-${z}B" $((5440 + z)) "-S 4m -o 5 -l $((z * 512)) -z $z -a 4099 -n 64" "-C 20" "-W 45"
        wait_dmabuf_gone 120
    done
    G1 "sudo sysctl -w net.ipv4.tcp_retries2=15"
}
"${1:-pos}"
