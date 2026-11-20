#!/bin/bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Ops-lock exercise for the ionic RX series. Run as root in a guest.
#   opslock.sh PEER_IP [ROUNDS]
# Each step logs its exit status; after every step the kernel log is checked.
IF=rocm-ernic0; PEER=${1:?peer ip}; ROUNDS=${2:-2}
BDF=$(basename "$(readlink /sys/class/net/$IF/device)")
MYIP=$(ip -4 -o addr show dev $IF | awk '{print $4}')
fail=0
splat() {
    local out
    out=$(dmesg | grep -E "BUG:|WARNING:|KASAN|possible (circular|recursive)|inconsistent lock|suspicious RCU|hung task|blocked for more|csum failure|page_pool_release_retry|stalled pool|Call Trace|refcount_t|DMA-API|sleeping function|held lock|RTNL: assertion")
    if [ -n "$out" ]; then echo "  !! SPLAT after: $1"; echo "$out" | head -n 12; fail=1; dmesg > /tmp/opslock-splat-"$(date +%s)".txt; dmesg -C; fi
}
step() { # description, command...
    local d=$1 rc; shift
    timeout 120 "$@" >/tmp/opslock.out 2>&1; rc=$?
    printf '%-58s rc=%s %s\n' "$d" "$rc" "$(head -c 150 /tmp/opslock.out | tr '\n' ' ')"
    [ $rc = 124 ] && { echo "  !! TIMEOUT (hang?): $d"; fail=1; }
    splat "$d"
}
UP="ip link set $IF up && ip addr replace $MYIP dev $IF"
up() { bash -c "$UP"; }
pingok() {
    local i
    for i in $(seq 50); do ping -c 1 -W 1 -I $IF "$PEER" >/dev/null 2>&1 && { echo "    ping ok ($1, try $i)"; return 0; }; sleep 0.2; done
    echo "  !! ping FAILED ($1)"; fail=1
}
[ -e /sys/fs/bpf/xdp_pass ] || python3 /mnt/hostfs/test-rx/guest/xdp_pin.py /sys/fs/bpf/xdp_pass
dmesg -C
echo "== $(uname -r) $(sha256sum /sys/module/ionic/sections/.text 2>/dev/null | cut -c1-8) rdma: $(rdma link 2>&1 | head -1)"
for r in $(seq "$ROUNDS"); do
    echo "== round $r"
    step "ifdown" ip link set $IF down
    step "ifup" bash -c "$UP"; pingok ifup
    step "ifdown/ifup x5" bash -c "for i in 1 2 3 4 5; do ip link set $IF down; ip link set $IF up; done"; up; pingok updown5
    step "ethtool -G rx 512 tx 512" ethtool -G $IF rx 512 tx 512; pingok ring512
    step "ethtool -G rx 4096 tx 4096" ethtool -G $IF rx 4096 tx 4096; pingok ring4096
    step "ethtool -L combined 1" ethtool -L $IF combined 1; pingok chan1
    step "ethtool -L combined 2" ethtool -L $IF combined 2; pingok chan2
    step "ethtool -L combined 4" ethtool -L $IF combined 4; pingok chan4
    step "mtu 9000" ip link set $IF mtu 9000; pingok mtu9000
    step "mtu 1500" ip link set $IF mtu 1500; pingok mtu1500
    step "ethtool -L rx 2 tx 2 (split interrupts)" ethtool -L $IF rx 2 tx 2 combined 0; pingok split-intr
    step "ifdown/ifup x3, split interrupts" bash -c "for i in 1 2 3; do ip link set $IF down; ip link set $IF up; done"; up; pingok split-intr-updown
    step "ethtool -G rx 1024 tx 1024, split interrupts" ethtool -G $IF rx 1024 tx 1024; pingok split-intr-ring
    step "mtu 3000, split interrupts" ip link set $IF mtu 3000; pingok split-intr-mtu
    step "mtu 1500, split interrupts" ip link set $IF mtu 1500
    step "ethtool -L combined 4 (shared again)" ethtool -L $IF combined 4 rx 0 tx 0; pingok chan4-again
    step "xdp attach (native)" ip link set dev $IF xdp pinned /sys/fs/bpf/xdp_pass; pingok xdp-on
    step "ethtool -L combined 2 with xdp" ethtool -L $IF combined 2; pingok xdp-chan2
    step "mtu 3000 with xdp (may be refused)" ip link set $IF mtu 3000
    step "mtu 1500 with xdp" ip link set $IF mtu 1500
    step "ifdown with xdp" ip link set $IF down
    step "ifup with xdp" bash -c "$UP"; pingok xdp-ifup
    step "xdp detach" ip link set dev $IF xdp off; pingok xdp-off
    step "ethtool -L combined 4" ethtool -L $IF combined 4
    step "ethtool -T" ethtool -T $IF
    step "hwtstamp get" ethtool --get-hwtimestamp-cfg $IF
    step "hwtstamp set (tsconfig netlink)" ethtool --set-hwtimestamp-cfg $IF rx-filter all tx on
    step "hwtstamp SIOC[GS]HWTSTAMP" python3 /mnt/hostfs/test-rx/guest/hwtstamp.py $IF
    step "ethtool get_link" ethtool $IF
    step "ethtool -c" ethtool -c $IF
    step "ethtool -C rx-usecs 50" ethtool -C $IF rx-usecs 50
    step "ethtool -C rx-usecs 3" ethtool -C $IF rx-usecs 3
    step "ethtool -a" ethtool -a $IF
    step "ethtool -A rx off tx off" ethtool -A $IF rx off tx off
    step "ethtool -x" ethtool -x $IF
    step "ethtool -X equal 2" ethtool -X $IF equal 2
    step "ethtool -X default" ethtool -X $IF default
    step "ethtool -s speed" ethtool -s $IF speed 100000 autoneg off
    step "ethtool --show-fec" ethtool --show-fec $IF
    step "ethtool --set-fec off" ethtool --set-fec $IF encoding off
    step "ethtool -r" ethtool -r $IF
    step "ethtool -m" ethtool -m $IF
    step "ethtool -S" ethtool -S $IF
    step "ethtool -d" ethtool -d $IF
    step "ethtool -k" ethtool -k $IF
    step "ethtool -K rx off" ethtool -K $IF rx off
    step "ethtool -K rx on" ethtool -K $IF rx on
    step "ethtool -K tso off gro off" ethtool -K $IF tso off gro off
    step "ethtool -K tso on gro on" ethtool -K $IF tso on gro on
    step "ethtool --show-tunable/priv" ethtool --show-priv-flags $IF
    step "promisc on" ip link set $IF promisc on
    step "promisc off" ip link set $IF promisc off
    step "allmulti on/off" bash -c "ip link set $IF allmulticast on; ip link set $IF allmulticast off"
    step "mc addr add/del" bash -c "ip maddr add 01:00:5e:01:02:03 dev $IF; ip maddr del 01:00:5e:01:02:03 dev $IF"
    step "mac change and back" bash -c "m=\$(cat /sys/class/net/$IF/address); ip link set $IF address 72:6f:63:6d:00:7$r; ip link set $IF address \$m"
    pingok after-ethtool
    step "rdma link / ibv_devinfo" bash -c "rdma link; ibv_devinfo | head -n 5"
    step "netdev genl page-pool-get dump" bash -c "ls /sys/class/net/$IF/queues | tr '\n' ' '"
    step "pci function reset (up)" bash -c "echo 1 > /sys/bus/pci/devices/$BDF/reset"
    up; pingok after-pci-reset-up
    step "ifdown" ip link set $IF down
    step "pci function reset (down)" bash -c "echo 1 > /sys/bus/pci/devices/$BDF/reset"
    step "ifup" bash -c "$UP"; pingok after-pci-reset-down
done
echo "== lockdep state: $(grep -E 'debug_locks' /proc/lockdep_stats)"
echo "== RESULT: $([ $fail = 0 ] && echo PASS || echo FAIL)"
exit $fail
