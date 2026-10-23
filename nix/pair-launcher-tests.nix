# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
{ pkgs }:
let
  fakeVm = node: pkgs.writeShellApplication {
    name = "fake-vm-${node}";
    text = ''exec ${pkgs.python3}/bin/python3 ${./tests/fake_vm.py} ${node}'';
  };
  runner = waitBoth: import ./pair-vms.nix {
    inherit pkgs waitBoth;
    vm1 = "${fakeVm "1"}/bin/fake-vm-1";
    vm2 = "${fakeVm "2"}/bin/fake-vm-2";
    shutdownTimeout = "0.2s";
    pollSeconds = "0.02";
  };
  port = pkgs.writeShellApplication {
    name = "normalize-pair-port";
    text = builtins.readFile ./pair-port.sh + ''normalize_pair_port "$1"'';
  };
in pkgs.runCommand "ernic-pair-launcher-tests" {
  nativeBuildInputs = [ pkgs.python3 ];
  ERNIC_TEST_INTERACTIVE = "${runner false}/bin/ernic-pair-vms";
  ERNIC_TEST_BOTH = "${runner true}/bin/ernic-pair-test-vms";
  ERNIC_TEST_PORT = "${port}/bin/normalize-pair-port";
} ''
  python3 ${./tests/test_pair_launchers.py}
  touch "$out"
''
