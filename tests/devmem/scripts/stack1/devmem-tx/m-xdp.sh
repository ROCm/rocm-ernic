#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# XDP on the sender's netdev while devmem TX runs. XDP_TX and XDP_REDIRECT
# frames use the same Tx descriptor-info buffer slots as skbs (patches 1-3 of
# the series), so the sender runs with ONE queue: every XDP frame and every
# devmem skb goes through queue 0. Guest 2 sends UDP to guest 1 port 7777; the
# XDP program on guest 1 echoes it and guest 2 checks every echo.
#   m-xdp.sh basic   pass, tx, redirect at MTU 1500 (single-buffer XDP)
#   m-xdp.sh frags   tx_frags, redirect_frags at MTU 9000 with 8000-byte datagrams (multi-buffer)
#   m-xdp.sh toggle  attach/detach programs while a devmem stream runs
# shellcheck source=tests/devmem/scripts/stack1/devmem-tx/lib.sh
. "$(dirname "$(readlink -f "$0")")/lib.sh"
STREAM="-S 16m -o 123 -l 100003 -a 7919 -n 0"
prep() { # MTU
    local n
    for n in 1 2; do "$S/gssh" $n "sudo ip link set $IF mtu $1; sudo install -m 0644 /mnt/hostfs/test/bin/xdp/*.o /mnt/hostfs/test/src/xdp/udp_echo_check.py /tmp/"; done
    G1 "sudo ethtool -L $IF combined 1; ethtool -l $IF | tail -n 2 | tr -s ' \n\t' ' '"; echo
}
attach() { # OBJ SEC
    G1 "sudo ip link set dev $IF xdpdrv obj /tmp/xdp_$1.o sec $2 2>&1; echo \"attach xdp_$1 rc=\$?\"; ip -d link show $IF | grep -o 'prog/xdp id [0-9]*'; for i in \$(seq 50); do ping -c 1 -W 1 -q $IP2 >/dev/null 2>&1 && break; sleep 0.2; done; ping -c 2 -q $IP2 | tail -n 2 | head -n 1"
}
detach() { G1 "sudo ip link set dev $IF xdp off; echo \"detach rc=\$?\"; for i in \$(seq 50); do ping -c 1 -W 1 -q $IP2 >/dev/null 2>&1 && break; sleep 0.2; done"; }
restore() { detach; local n; for n in 1 2; do "$S/gssh" $n "sudo ip link set $IF mtu 1500"; done; G1 "sudo ethtool -L $IF combined 4; ethtool -l $IF | tail -n 2 | tr -s ' \n\t' ' '"; echo; }
one() { # NAME PORT OBJ SEC UDPSIZE
    attach "$3" "$4"
    RXWAIT=60 HOOK_DELAY=2 HOOK="G2 'python3 /tmp/udp_echo_check.py $IP1 10 $5 1000'" \
        devmem_case "$1" "$2" "$STREAM" "-T 15 -u 5000 -C 30" "-W 30"
    detach
}
case "$1" in
basic)
    prep 1500
    one XDP-pass-with-devmem-tx 5500 pass xdp 1000
    one XDP-TX-with-devmem-tx 5501 tx xdp 1000
    one XDP-REDIRECT-with-devmem-tx 5502 redirect xdp 1000
    restore ;;
frags)
    prep 9000
    one XDP-TX-multibuf-with-devmem-tx-mtu9000 5503 tx_frags xdp.frags 8000
    one XDP-REDIRECT-multibuf-with-devmem-tx-mtu9000 5504 redirect_frags xdp.frags 8000
    restore ;;
toggle)
    prep 1500
    RXWAIT=90 HOOK_DELAY=3 HOOK="G1 'for o in pass tx redirect; do sudo ip link set dev $IF xdpdrv obj /tmp/xdp_\$o.o sec xdp; echo \"attach \$o rc=\$?\"; sleep 2; sudo ip link set dev $IF xdp off; echo \"detach rc=\$?\"; sleep 2; done'" \
        devmem_case XDP-attach-detach-mid-stream 5505 "$STREAM" "-T 25 -u 5000 -R 20 -C 30" "-W 30"
    restore ;;
esac
