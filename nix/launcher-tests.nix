# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
{ pkgs }:
let
  inherit (pkgs) lib;
  fakeServer = pkgs.writeShellApplication {
    name = "rocm-ernic";
    text = ''exec ${pkgs.python3}/bin/python3 ${./tests/fake_ernic.py} server "$@"'';
  };
  runner = import ./ernic-run.nix {
    inherit pkgs;
    rocm-ernic = fakeServer;
    startupAttempts = 20;
    pollSeconds = "0.02";
    shutdownTimeout = "0.4s";
  };
  # Evaluate the reusable module against its declared option dependencies.
  # This tests our assertions without building a kernel or guest VM.
  evaluate = enable: cores: version: lib.evalModules {
    specialArgs = { inherit pkgs; };
    modules = [ ./guest-module.nix ({ lib, ... }: {
      options = {
        assertions = lib.mkOption { type = lib.types.listOf lib.types.attrs; default = []; };
        virtualisation.cores = lib.mkOption { type = lib.types.int; };
        virtualisation.qemu = lib.mkOption { type = lib.types.attrs; default = {}; };
        boot = lib.mkOption { type = lib.types.attrs; };
        systemd = lib.mkOption { type = lib.types.attrs; default = {}; };
        environment = lib.mkOption { type = lib.types.attrs; default = {}; };
      };
      config = {
        virtualisation.ernic.enable = enable;
        virtualisation.cores = cores;
        boot.kernelPackages.kernel.version = version;
      };
    }) ];
  };
  assertionsPass = enable: cores: version:
    lib.all (a: a.assertion) (evaluate enable cores version).config.assertions;
  cases = [
    { description = "positive: minimum supported guest"; cores = 4; version = "6.18"; enable = true; expected = true; }
    { description = "negative: old kernel"; cores = 4; version = "6.17"; enable = true; expected = false; }
    { description = "boundary: one core too few"; cores = 3; version = "6.18"; enable = true; expected = false; }
    { description = "corner: disabled module"; cores = 1; version = "6.17"; enable = false; expected = true; }
  ];
in
assert lib.all (c: lib.assertMsg
  (assertionsPass c.enable c.cores c.version == c.expected) c.description) cases;
pkgs.runCommand "ernic-launcher-tests" {
  nativeBuildInputs = [ pkgs.python3 ];
  ERNIC_TEST_RUNNER = "${runner}/bin/ernic-run";
  ERNIC_TEST_BASH = "${pkgs.bash}/bin/bash";
  ERNIC_TEST_QEMU_OPTIONS = pkgs.writeText "guest-options.json"
    (builtins.toJSON (evaluate true 4 "6.18").config.virtualisation.qemu.options);
} ''
  python3 ${./tests}/test_launchers.py
  touch "$out"
''
