# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
# The same supervisor serves real VMs and controlled subprocess fixtures.
{ pkgs, vm1, vm2, waitBoth ? false, shutdownTimeout ? "10s", pollSeconds ? "0.1" }:
pkgs.writeShellApplication {
  name = if waitBoth then "ernic-pair-test-vms" else "ernic-pair-vms";
  runtimeInputs = [ pkgs.coreutils pkgs.util-linux ];
  text = builtins.readFile ./pair-port.sh + ''
    ssh_port=""
    if [ -n "''${1:-}" ]; then ssh_port=$(normalize_pair_port "$1"); fi
    net() {
      if [ -n "$ssh_port" ]; then
        echo "hostfwd=tcp:127.0.0.1:$((ssh_port + $1 - 1))-:22"
      fi
    }
    vm1_pid=""
    vm2_pid=""
    stop_group() {
      local pid="$1"
      kill -TERM -- "-$pid" 2>/dev/null || true
      timeout --kill-after=1s ${shutdownTimeout} tail --sleep-interval=0.05 --pid="$pid" -f /dev/null || true
      kill -KILL -- "-$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    }
    cleanup() {
      if [ -n "$vm1_pid" ]; then stop_group "$vm1_pid"; fi
      if [ -n "$vm2_pid" ]; then stop_group "$vm2_pid"; fi
    }
    trap 'cleanup' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    mkdir -p "$ERNIC_PAIR_DIR/node1/shared" "$ERNIC_PAIR_DIR/node2/shared"
    setsid env TMPDIR="$ERNIC_PAIR_DIR/node2" SHARED_DIR="$ERNIC_PAIR_DIR/node2/shared" QEMU_NET_OPTS="$(net 2)" \
      ${vm2} < /dev/null > "$ERNIC_PAIR_DIR/serial-2.log" 2>&1 &
    vm2_pid=$!
    setsid env TMPDIR="$ERNIC_PAIR_DIR/node1" SHARED_DIR="$ERNIC_PAIR_DIR/node1/shared" QEMU_NET_OPTS="$(net 1)" \
      ${vm1} <&0 ${pkgs.lib.optionalString waitBoth ''> "$ERNIC_PAIR_DIR/serial-1.log" 2>&1''} &
    vm1_pid=$!
    done1=0
    done2=0
    while [ "$done1" = 0 ] || [ "$done2" = 0 ]; do
      if [ "$done1" = 0 ] && ! kill -0 "$vm1_pid" 2>/dev/null; then
        rc=0
        wait "$vm1_pid" || rc=$?
        done1=1
        [ "$rc" = 0 ] || exit "$rc"
        ${pkgs.lib.optionalString (!waitBoth) "break"}
      fi
      if [ "$done2" = 0 ] && ! kill -0 "$vm2_pid" 2>/dev/null; then
        rc=0
        wait "$vm2_pid" || rc=$?
        done2=1
        [ "$rc" = 0 ] || exit "$rc"
        ${pkgs.lib.optionalString (!waitBoth) "exit 1"}
      fi
      sleep ${pollSeconds}
    done
    cleanup
    trap - EXIT
  '';
}
