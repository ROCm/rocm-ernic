# nix/microvms/mkVm.nix
#
# A NixOS microvm that boots under QEMU and runs the cross-built rocm-ernic
# server against the PCI-config test client on the loopback backend — the
# emulated-run counterpart of the native loopback exercise in nix/analysis/.
#
# Returns `.config.microvm.declaredRunner`: a package whose bin/microvm-run
# launches QEMU. A oneshot systemd service (ernic-self-test) runs the
# exercise at boot and prints `ERNIC Self-Test: SUCCESS` / `FAIL`; the
# lifecycle driver (nix/microvms/lib.nix) watches for that marker.
#
# Modeled closely on ~/Downloads/xdp2/nix/microvms/mkVm.nix — same TCG /
# seccomp-off QEMU / 9p-store / hand-built -append mechanics, which are the
# fiddly, proven bits — but trimmed to a userspace socket server (no
# eBPF/BTF/XDP). rocm-ernic itself and the runner scripts are built from the
# VM's own (target-arch) pkgs so their store paths are the guest's and run
# natively inside the guest over the 9p-mounted /nix/store.
#
# Usage:
#   import ./mkVm.nix {
#     inherit pkgs lib microvm nixpkgs buildSystem arch libvfioUserSrc;
#   }

{ pkgs, lib, microvm, nixpkgs, arch, buildSystem, libvfioUserSrc }:

let
  constants = import ./constants.nix;
  cfg = constants.architectures.${arch};
  hostname = constants.getHostname arch;
  ports = constants.getPorts arch;

  # QEMU without seccomp: the -sandbox option added when seccomp is on does
  # not work for cross-arch (TCG) targets.
  qemuWithoutSandbox = pkgs.qemu.override { seccompSupport = false; };

  # TCG acceleration for the emulated arches (machineOpts is how microvm.nix
  # threads `accel=` into non-built-in machine types).
  machineOpts = if cfg.useKvm then null else { accel = "tcg"; };

  needsCross = buildSystem != cfg.nixSystem;

  # Overlay to disable test phases for deps whose suites fail under cross /
  # emulation. Starts empty; the spike adds entries with the exact error, as
  # xdp2 does (e.g. `somePkg = prev.somePkg.overrideAttrs (_: { doCheck = false; });`).
  crossEmulationOverlay = final: prev: {
  };

  overlayedPkgs = import nixpkgs (
    (if needsCross
     then { localSystem = buildSystem; crossSystem = cfg.nixSystem; }
     else { system = cfg.nixSystem; })
    // { overlays = [ crossEmulationOverlay ]; config = { allowUnfree = true; }; }
  );

  # Build rocm-ernic (+ test_pci_client) from the VM's own pkgs, so the
  # binaries are the guest architecture and 9p-visible inside the VM.
  ernicCtx = import ../lib.nix {
    pkgs = overlayedPkgs;
    inherit lib;
    libvfio-user = import ../libvfio-user.nix {
      inherit lib;
      inherit (overlayedPkgs) stdenv meson ninja pkg-config json_c cmocka;
      src = libvfioUserSrc;
    };
  };
  ernic = import ../derivation.nix {
    ctx = ernicCtx;
    src = ../..;
    installTests = true;
  };

  # The loopback exercise driver (shared with the native analysis targets),
  # built from the guest pkgs so its interpreter is the guest's.
  loopback = import ../analysis/run-loopback.nix { pkgs = overlayedPkgs; };

  # The self-test: run the exercise, then gate on the client's PASS marker
  # (tests/test_pci_client.c prints "✓ Test PASSED" and exits 0 on success).
  selfTest = overlayedPkgs.writeShellApplication {
    name = "ernic-self-test";
    runtimeInputs = [ overlayedPkgs.coreutils overlayedPkgs.gnugrep ];
    text = ''
      echo "=================================================="
      echo "  ROCm ERNIC MicroVM Self-Test"
      echo "=================================================="
      echo "Architecture: $(uname -m)   Kernel: $(uname -r)"
      echo ""

      export TMPDIR=/tmp
      export OUT_LOG=/tmp/ernic-server.log
      export CLIENT_LOG=/tmp/ernic-client.log

      "${loopback}/bin/run-loopback-exercise" \
        "${ernic}/bin/rocm-ernic" "${ernic}/bin/test_pci_client"

      echo "--- server log (tail) ---"
      tail -n 20 "$OUT_LOG" 2>/dev/null || true
      echo "--- client log ---"
      cat "$CLIENT_LOG" 2>/dev/null || true
      echo ""

      if grep -q "Test PASSED" "$CLIENT_LOG" 2>/dev/null; then
        echo "ERNIC Self-Test: SUCCESS"
      else
        echo "ERNIC Self-Test: FAIL"
        exit 1
      fi
    '';
  };
in
(nixpkgs.lib.nixosSystem {
  pkgs = overlayedPkgs;
  specialArgs = { inherit overlayedPkgs; };

  modules = [
    microvm.nixosModules.microvm

    # Force our pre-overlayed pkgs everywhere (otherwise the nixpkgs module
    # re-imports without the overlay). Same mechanism as the xdp2 reference.
    ({ lib, ... }: {
      _module.args.pkgs = lib.mkForce overlayedPkgs;
      nixpkgs.pkgs = lib.mkForce overlayedPkgs;
      nixpkgs.hostPlatform = lib.mkForce overlayedPkgs.stdenv.hostPlatform;
      nixpkgs.buildPlatform = lib.mkForce overlayedPkgs.stdenv.buildPlatform;
    })

    ({ config, pkgs, ... }: {
      # Minimal system: trim everything a headless socket-server test does
      # not need, to keep the (cross-compiled) closure small.
      documentation.enable = false;
      documentation.man.enable = false;
      documentation.doc.enable = false;
      documentation.info.enable = false;
      documentation.nixos.enable = false;
      security.polkit.enable = false;
      services.udisks2.enable = false;
      programs.command-not-found.enable = false;
      fonts.fontconfig.enable = false;
      nix.enable = false;
      xdg.mime.enable = false;
      boot.supportedFilesystems = lib.mkForce [ "vfat" "ext4" ];
      hardware.enableRedistributableFirmware = false;

      system.stateVersion = "26.05";
      networking.hostName = hostname;

      microvm = {
        hypervisor = "qemu";
        mem = constants.getMem arch;
        vcpu = constants.vcpu;
        # Explicit CPU on TCG arches so microvm.nix does not add -enable-kvm
        # (which it does when cpu == null on a Linux host).
        cpu = if cfg.useKvm then null else cfg.qemuCpu;
        volumes = [ ];

        # 9p-mount the host store so the guest sees every built store path.
        shares = [{
          source = "/nix/store";
          mountPoint = "/nix/store";
          tag = "nix-store";
          proto = "9p";
        }];

        qemu = {
          serialConsole = false;
          machine = cfg.qemuMachine;
          package = if cfg.useKvm then pkgs.qemu_kvm else qemuWithoutSandbox;

          extraArgs = cfg.biosArgs ++ [
            "-name" "${hostname},process=${hostname}"
            "-serial" "tcp:127.0.0.1:${toString ports.serial},server,nowait"
            "-device" "virtio-serial-pci"
            "-chardev" "socket,id=virtcon,port=${toString ports.virtio},host=127.0.0.1,server=on,wait=off"
            "-device" "virtconsole,chardev=virtcon"
            # microvm.nix omits -append for non-microvm machine types, so we
            # supply the kernel cmdline (incl. init=) by hand. The serial
            # device is listed LAST so it becomes /dev/console — the self-test
            # service mirrors its verdict there (StandardOutput=journal+console)
            # and the host-side driver watches that serial TCP port.
            "-append" (builtins.concatStringsSep " " ([
              "console=hvc0"
              "console=${cfg.consoleDevice},115200"
              "reboot=t"
              "panic=-1"
              "loglevel=4"
              "init=${config.system.build.toplevel}/init"
            ] ++ config.boot.kernelParams))
          ];
        } // lib.optionalAttrs (machineOpts != null) { inherit machineOpts; };
      };

      # Console ordering lives entirely in the QEMU -append above (serial
      # last => /dev/console). Keep only non-console kernel params here; they
      # are appended after the -append list, so they must not add a console=.
      boot.kernelParams = [
        "systemd.default_standard_error=journal+console"
        "systemd.show_status=true"
      ];
      boot.initrd.availableKernelModules = [
        "9p" "9pnet" "9pnet_virtio" "virtio_pci" "virtio_console"
      ];
      # Keep booting rather than dropping to a locked emergency shell.
      boot.initrd.systemd.emergencyAccess = true;
      systemd.enableEmergencyMode = false;

      services.getty.autologinUser = "root";
      # Precomputed hash for the password "test" (usable at the console).
      users.users.root.hashedPassword =
        "$6$xyz$LH8r4wzLEMW8IaOSNSaJiXCrfvBsXKjJhBauJQIFsT7xbKkNdM0xQx7gQZt.z6G.xj2wX0qxGm.7eVxJqkDdH0";

      environment.systemPackages = with pkgs; [
        coreutils procps util-linux gnugrep selfTest
      ];

      systemd.services.ernic-self-test = {
        description = "ROCm ERNIC MicroVM Self-Test";
        after = [ "multi-user.target" ];
        wantedBy = [ "multi-user.target" ];
        serviceConfig = {
          Type = "oneshot";
          RemainAfterExit = true;
          ExecStart = "${selfTest}/bin/ernic-self-test";
          # Mirror the SUCCESS/FAIL marker onto the serial console so the
          # host-side lifecycle driver (nix/microvms/lib.nix) can watch for it.
          StandardOutput = "journal+console";
          StandardError = "journal+console";
        };
      };
    })
  ];
}).config.microvm.declaredRunner
