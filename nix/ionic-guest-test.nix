# nix/ionic-guest-test.nix
#
# packages.run-ionic-guest-test: boot a NixOS qemu-vm.nix guest with
# nixosModules.ernic-guest against a loopback rocm-ernic server, and check
# that the in-tree ionic/ionic_rdma drivers bind and the RDMA port reaches
# PORT_ACTIVE. This exercises the consumer path other flakes use.
#
#   nix run .#run-ionic-guest-test              # KVM if usable, else TCG
#   nix run .#run-ionic-guest-test -- --accel=tcg
#
# Needs a host where QEMU can run (/dev/kvm for speed); no root, no
# /dev/vfio and no RDMA hardware. Evidence lands in a mktemp directory.

{ pkgs, lib, nixpkgs, ernicRun, guestModule }:

let
  guestCheck = pkgs.writeShellApplication {
    name = "ernic-ionic-guest-check";
    runtimeInputs = with pkgs; [ coreutils gnugrep iproute2 pciutils rdma-core systemd ];
    text = ''
      trap 'echo ERNIC_GUEST_FAIL; systemctl poweroff --no-block' ERR
      lspci -n | grep -q '1dd8:1002'
      echo ERNIC_GUEST_PCI_PASS
      # ernic-rdma-up (guest module) opens the netdev and loads ionic_rdma;
      # the RoCE port then follows the netdev to ACTIVE.
      systemctl is-active --quiet ernic-rdma-up.service
      echo ERNIC_GUEST_RDMA_UP_PASS
      for _ in $(seq 1 60); do
        rdma link show | grep -q 'state ACTIVE' && break
        sleep 1
      done
      rdma link show
      rdma link show | grep -q 'state ACTIVE'
      echo ERNIC_GUEST_RDMA_LINK_PASS
      ibv_devinfo | grep -q PORT_ACTIVE
      echo ERNIC_GUEST_PORT_ACTIVE_PASS
      echo ERNIC_GUEST_PASS
      systemctl poweroff --no-block
    '';
  };

  vm =
    (nixpkgs.lib.nixosSystem {
      system = pkgs.stdenv.hostPlatform.system;
      modules = [
        "${nixpkgs}/nixos/modules/virtualisation/qemu-vm.nix"
        guestModule
        {
          nixpkgs.pkgs = pkgs;
          system.stateVersion = "26.05";
          networking.hostName = "ernic-ionic-guest";
          networking.useDHCP = false;
          boot.kernelPackages = pkgs.linuxPackages_latest;
          virtualisation = {
            ernic.enable = true;
            graphics = false;
            memorySize = 2048;
            cores = 4;
            diskImage = null;
            qemu.networkingOptions = lib.mkForce [ ];
            qemu.options = [ "-no-reboot" ];
          };
          systemd.services.ernic-ionic-guest-check = {
            wantedBy = [ "multi-user.target" ];
            wants = [ "ernic-rdma-up.service" ];
            after = [ "ernic-rdma-up.service" ];
            serviceConfig = {
              Type = "oneshot";
              ExecStart = "${guestCheck}/bin/ernic-ionic-guest-check";
              StandardOutput = "journal+console";
              StandardError = "journal+console";
            };
          };
        }
      ];
    }).config.system.build.vm;
in
pkgs.writeShellApplication {
  name = "run-ionic-guest-test";
  runtimeInputs = with pkgs; [ coreutils gnugrep ];
  text = ''
    case "''${1:---accel=auto}" in
      --accel=auto) export QEMU_OPTS='-machine accel=kvm:tcg' ;;
      --accel=kvm) export QEMU_OPTS='-machine accel=kvm' ;;
      --accel=tcg) export QEMU_OPTS='-machine accel=tcg' ;;
      *) echo 'usage: run-ionic-guest-test [--accel=auto|kvm|tcg]' >&2; exit 2 ;;
    esac
    run_dir=$(mktemp -d -t ernic-ionic-guest-XXXXXX)
    echo "Evidence: $run_dir"
    cd "$run_dir"
    export TMPDIR="$run_dir"
    unset QEMU_KERNEL_PARAMS QEMU_NET_OPTS NIX_DISK_IMAGE
    rc=0
    timeout --kill-after=10s 15m \
      ${ernicRun}/bin/ernic-run --socket "$run_dir/ernic.sock" -- \
      ${vm}/bin/run-ernic-ionic-guest-vm > "$run_dir/serial.log" 2>&1 || rc=$?
    cat "$run_dir/serial.log"
    if [ "$rc" -ne 0 ]; then
      echo "FAIL: launcher exited $rc; evidence $run_dir" >&2
      exit 1
    fi
    for sentinel in PCI_PASS RDMA_UP_PASS RDMA_LINK_PASS PORT_ACTIVE_PASS PASS; do
      if ! grep -q "ERNIC_GUEST_$sentinel" "$run_dir/serial.log"; then
        echo "FAIL: missing ERNIC_GUEST_$sentinel; evidence $run_dir" >&2
        exit 1
      fi
    done
    if grep -q ERNIC_GUEST_FAIL "$run_dir/serial.log"; then
      echo "FAIL: guest check failed; evidence $run_dir" >&2
      exit 1
    fi
    echo "PASS: ionic guest reached PORT_ACTIVE; evidence $run_dir"
  '';
}
