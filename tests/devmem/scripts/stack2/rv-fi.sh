#!/usr/bin/env bash
# Copyright (C) Advanced Micro Devices, Inc.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or later.
# See the LICENSE_GPL.md file in the top-level directory.
#
# Review round: queue-op failure paths, FW down/up, Tx timeout work and link down, driven by the
# TEST-ONLY knobs of the rx5fi module (patches-rx/extra/test-only-ionic-fault-knobs.diff) in
# guest 1. The emulated firmware stays alive throughout: "FW down" is the driver's own
# ionic_lif_handle_fw_down()/fw_up() run as the heartbeat check would run them.
#   rv-fi.sh CASE...   (qfail1 qfail2 qfail-unbind fw fw-bind fw-hold qop-fw txto link)
# shellcheck source=tests/devmem/scripts/stack2/tdlib.sh
. "$(dirname "$(readlink -f "$0")")/tdlib.sh"
IF=rocm-ernic0; P=/sys/module/ionic/parameters
knob() { g 1 "echo $2 | sudo tee $P/$1 >/dev/null"; }
dm() { g 1 "sudo dmesg | grep -E 'ionic fi|FW Down|FW Up|FW running|FW stopped|Failed to restart|Link (up|down)|Restarting queues|Failed to start|WARNING|BUG|KASAN' | cut -c1-150 | tail -n ${1:-12}"; }
state() { g 1 "echo \"   present=\$(cat /sys/class/net/$IF/device/net/$IF/operstate 2>/dev/null) flags=\$(ip -o link show $IF | grep -o '<[^>]*>') ethtool-g-rc=\$(ethtool -g $IF >/dev/null 2>&1; echo \$?)\""; }
pingw() { g 1 "for i in \$(seq ${1:-50}); do ping -c1 -W1 $IP2 >/dev/null 2>&1 && { echo \"   ping ok (try \$i)\"; exit 0; }; sleep 0.2; done; echo '   ping FAILED'"; }
updown() { g 1 "sudo ip link set $IF down; sudo ip link set $IF up; sudo ip addr replace $IP1/24 dev $IF"; }
wait_fw_up() { g 1 "for i in \$(seq 150); do sudo dmesg | grep -q 'FW Up: LIFs restarted' && { echo \"   FW up after \$i tries\"; exit 0; }; sleep 0.2; done; echo '   !! FW never came up'"; }
xf() { "$S/xfer.sh" 2 1 "$IP1" "$IP1" 8000000 bulk; "$S/xfer.sh" 1 2 "$IP2" "$IP2" 8000000 bulk; }
fin() { g 1 'sudo pkill -x ncdevmem; sleep 0.5'; wait_no_dmabuf_pool 1; "$S/splat.sh" 1; echo "   lockdep rx-1: $(g 1 "sudo grep debug_locks /proc/lockdep_stats | tr -s ' '")"; }
for c in "$@"; do
"$S/td-setup.sh" 1 >/dev/null
case $c in
qfail1)
    hdr "qfail1: ndo_queue_start() fails once at bind; the core restarts the old queue"
    knob fi_qstart_fail 1
    "$S/devmem-rx.sh" start 1 | tail -n 3
    dm; state; pools 1; pingw; xf
    echo "-- bind again, no fault:"; first_light 1048576
    fin ;;
qfail2)
    hdr "qfail2: ndo_queue_start() fails for the new and for the old queue at bind (core WARNs: expected)"
    knob fi_qstart_fail 2
    "$S/devmem-rx.sh" start 1 | tail -n 3
    dm; state; pingw 10
    echo "-- 7 s later (a watchdog link check has run):"; sleep 7; dm 4; state; pingw 5
    echo "-- ifdown/ifup:"; updown; state; pingw; xf
    echo "-- bind again, no fault:"; first_light 1048576
    g 1 'sudo dmesg -C'; fin ;;
qfail-unbind)
    hdr "qfail-unbind: ndo_queue_start() fails once at unbind (core WARN_ON: expected)"
    "$S/devmem-rx.sh" start 1 | tail -n 1
    knob fi_qstart_fail 1
    g 1 'sudo pkill -x ncdevmem; sleep 1'
    dm; state; pools 1; pingw 10
    echo "-- ifdown/ifup:"; updown; state; pools 1; pingw; xf
    echo "-- bind again, no fault:"; first_light 1048576
    g 1 'sudo dmesg -C'; fin ;;
fw)
    hdr "fw: FW down/up, idle, then under bidirectional TCP"
    knob fi_cmd fwdown; wait_fw_up; dm; state; pingw; xf
    g 1 'sudo dmesg -C'
    ("$S/xfer.sh" 2 1 "$IP1" "$IP1" 400000000 bulk; PORT=5311 "$S/xfer.sh" 1 2 "$IP2" "$IP2" 400000000 bulk) & sleep 2
    knob fi_cmd fwdown; wait_fw_up; dm; wait; pingw; xf
    fin ;;
fw-bind)
    hdr "fw-bind: FW down/up with a live binding, mid-stream"
    "$S/devmem-rx.sh" start 1 | tail -n 1; stream_bg 1000000000; wait_rx_bytes 20000000
    knob fi_cmd fwdown; wait_fw_up; dm; pools 1; pingw
    # shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
    g 2 'for i in $(seq 600); do [ -e /tmp/sender.rc ] && break; sleep 0.2; done; cat /tmp/sender.rc 2>/dev/null || { echo "sender still running; killing"; pkill socat; }'
    "$S/devmem-rx.sh" wait 1 100
    echo "-- bind again:"; first_light 1048576
    fin ;;
fw-hold)
    hdr "fw-hold: FW down and held down; unbind, ethtool, Tx timeout work while down; then FW up"
    "$S/devmem-rx.sh" start 1 | tail -n 1
    knob fi_cmd fwdown-hold
    g 1 "for i in \$(seq 100); do sudo dmesg | grep -q 'FW Down: LIFs stopped' && break; sleep 0.2; done"; dm; state
    g 1 "sudo pkill -x ncdevmem; sleep 1; pgrep -x ncdevmem >/dev/null && echo '   ncdevmem still alive' || echo '   ncdevmem gone (unbind ran with the FW down)'"
    knob fi_cmd txtimeout
    g 1 "sudo ethtool -G $IF rx 2048; echo \"   ethtool -G while down rc=\$?\"; sudo ethtool -L $IF combined 2; echo \"   ethtool -L while down rc=\$?\"; sudo ip link set $IF mtu 1400; echo \"   mtu while down rc=\$?\""
    echo "-- bind while the FW is down (must fail):"; "$S/devmem-rx.sh" start 1 | tail -n 2; g 1 'sudo pkill -x ncdevmem'
    knob fi_cmd fwup; wait_fw_up; dm; state; pools 1
    g 1 "sudo ip link set $IF mtu 1500; sudo ethtool -L $IF combined 1; sudo ethtool -G $IF rx 1024"; pingw; xf
    echo "-- bind again:"; first_light 1048576
    fin ;;
qop-fw)
    hdr "qop-fw: a FW reset begins between ndo_queue_stop() and ndo_queue_start() of a bind"
    knob fi_qop_fw_reset 1
    "$S/devmem-rx.sh" start 1 | tail -n 2; wait_fw_up; dm; state; pools 1; pingw
    send_fg 16777216; "$S/devmem-rx.sh" wait 1 100
    wait_no_dmabuf_pool 1; g 1 'sudo dmesg -C'
    hdr "qop-fw: the same at unbind"
    "$S/devmem-rx.sh" start 1 | tail -n 1
    knob fi_qop_fw_reset 1
    g 1 'sudo pkill -x ncdevmem; sleep 1'; wait_fw_up; dm; state; pools 1; pingw; xf
    echo "-- bind again:"; first_light 1048576
    fin ;;
txto)
    hdr "txto: Tx timeout work, idle, under TCP, and with a binding mid-stream"
    knob fi_cmd txtimeout; sleep 1; dm 4; pingw; xf
    ("$S/xfer.sh" 2 1 "$IP1" "$IP1" 200000000 bulk) & sleep 1; knob fi_cmd txtimeout; wait; pingw
    "$S/devmem-rx.sh" start 1 | tail -n 1; stream_bg 300000000; wait_rx_bytes 20000000
    knob fi_cmd txtimeout; sleep 1; pools 1
    # shellcheck disable=SC2016 # for the guest's shell (or awk there) to expand
    g 2 'for i in $(seq 600); do [ -e /tmp/sender.rc ] && break; sleep 0.2; done; cat /tmp/sender.rc 2>/dev/null || { echo "sender still running; killing"; pkill socat; }'
    "$S/devmem-rx.sh" wait 1 100
    fin ;;
link)
    hdr "link: link down and up through the link check, idle, then with a binding; bind while the link is down"
    knob fi_link_down 1; knob fi_cmd linkcheck; sleep 1; dm 4; g 1 "ip -br link show $IF"; pingw 5
    knob fi_link_down 0; knob fi_cmd linkcheck; sleep 1; dm 4; pingw; xf
    "$S/devmem-rx.sh" start 1 | tail -n 1
    knob fi_link_down 1; knob fi_cmd linkcheck; sleep 1; g 1 "ip -br link show $IF"
    knob fi_link_down 0; knob fi_cmd linkcheck; sleep 1; pingw; pools 1
    send_fg 16777216; "$S/devmem-rx.sh" wait 1 100; wait_no_dmabuf_pool 1
    echo "-- bind while the link is down, then link up:"
    knob fi_link_down 1; knob fi_cmd linkcheck; sleep 1
    "$S/devmem-rx.sh" start 1 | tail -n 1; pools 1
    knob fi_link_down 0; knob fi_cmd linkcheck; sleep 1; pingw
    send_fg 16777216; "$S/devmem-rx.sh" wait 1 100
    fin ;;
esac
done
