# nix/guest-module.nix
#
# NixOS module for a qemu-vm.nix guest that gets the emulated ionic NIC.
# Exported as nixosModules.ernic-guest, so another flake can boot a guest
# with a real ionic/ionic_rdma driver path and no RDMA hardware:
#
#   modules = [
#     "${nixpkgs}/nixos/modules/virtualisation/qemu-vm.nix"
#     rocm-ernic.nixosModules.ernic-guest
#     { virtualisation.ernic.enable = true; }
#   ];
#
# The host side starts the server first; packages.ernic-run does that and
# exports ERNIC_SOCKET for the VM launcher (see nix/ernic-run.nix).
#
# What it sets, and why:
# - memory-backend-memfd,share=on: the server DMAs guest memory, so guest RAM
#   must be shared (qemu.enableSharedMemory).
# - q35 plus a pcie-root-port: vfio-user-pci sits behind rp0 so QMP
#   device_del/device_add can hot-unplug and replug it.
# - at least 4 vCPUs: with fewer, ionic_rdma cannot get IONIC_EQ_COUNT_MIN
#   event queues and only logs "Failed to register ibdev".
# - the in-tree ionic and ionic_rdma modules (kernel >= 6.18); the UC
#   address-handle patch in patches/ only matters for UC queue pairs.
# - netdev up before ionic_rdma: ionic_lif_size() reserves 1 + nxqs + neqs
#   interrupts, but ionic_rdma creates EQs until ionic_intr_alloc() fails, so
#   loaded first it takes the TxRx vectors and "ip link set up" then fails
#   with "qcq alloc of rx0 failed -28". ionic_rdma is kept off alias
#   autoload and the ernic-rdma-up service opens the netdev, then loads it.

{ config, lib, pkgs, ... }:

let
  cfg = config.virtualisation.ernic;
  # Preserve documented shell variable expansion while quoting literal path
  # characters. jq performs JSON escaping after the shell expands the path.
  socketExpr = builtins.replaceStrings [ "\\" "\"" "`" ] [ "\\\\" "\\\"" "\\`" ] cfg.socket;

  rdmaUp = pkgs.writeShellApplication {
    name = "ernic-rdma-up";
    runtimeInputs = with pkgs; [ coreutils iproute2 kmod systemd ];
    text = ''
      modprobe ionic
      netdev=""
      for _ in $(seq 1 30); do
        udevadm settle
        for path in /sys/bus/pci/drivers/ionic/*/net/*; do
          [ -e "$path" ] && netdev=''${path##*/} && break
        done
        [ -n "$netdev" ] && break
        sleep 1
      done
      if [ -z "$netdev" ]; then
        echo 'ernic-rdma-up: no ionic netdev after 30s' >&2
        exit 1
      fi
      ip link set "$netdev" up
      modprobe ionic_rdma
      echo "ernic-rdma-up: $netdev up, ionic_rdma loaded"
    '';
  };
in
{
  options.virtualisation.ernic = {
    enable = lib.mkEnableOption "the rocm-ernic emulated ionic NIC (vfio-user-pci)";

    socket = lib.mkOption {
      type = lib.types.str;
      default = "\${ERNIC_SOCKET:?ERNIC_SOCKET must name the rocm-ernic socket}";
      description = ''
        vfio-user socket path. It is expanded by the VM launcher's shell, so
        the default reads ERNIC_SOCKET at launch, as exported by ernic-run.
      '';
    };

    deviceId = lib.mkOption {
      type = lib.types.str;
      default = "ernic0";
      description = "QEMU device id, the handle for QMP device_del/device_add.";
    };

    rootPort = lib.mkOption {
      type = lib.types.str;
      default = "rp0";
      description = "id of the pcie-root-port the device is plugged into.";
    };
  };

  config = lib.mkIf cfg.enable {
    assertions = [
      {
        assertion = config.virtualisation.cores >= 4;
        message = "virtualisation.ernic needs virtualisation.cores >= 4 (ionic EQ minimum).";
      }
      {
        assertion = lib.versionAtLeast config.boot.kernelPackages.kernel.version "6.18";
        message = "virtualisation.ernic needs a guest kernel >= 6.18 for the in-tree ionic_rdma driver.";
      }
    ];

    virtualisation.qemu.enableSharedMemory = true;
    virtualisation.qemu.options = [
      "-machine q35"
      "-device pcie-root-port,id=${cfg.rootPort},chassis=1,slot=1"
      # A single JSON argv element, including runtime paths with quotes or
      # backslashes; shell quoting alone does not make a JSON string safe.
      "-device \"$(${pkgs.jq}/bin/jq -cn --arg path \"${socketExpr}\" --arg id ${lib.escapeShellArg cfg.deviceId} --arg bus ${lib.escapeShellArg cfg.rootPort} '{driver: \"vfio-user-pci\", id: $id, bus: $bus, socket: {type: \"unix\", path: $path}}')\""

    ];

    boot.kernelModules = [ "ionic" "ib_uverbs" ];
    # Only blocks alias autoload; ernic-rdma-up's explicit modprobe still works.
    boot.blacklistedKernelModules = [ "ionic_rdma" ];
    systemd.services.ernic-rdma-up = {
      description = "Open the ionic netdev, then load ionic_rdma";
      wantedBy = [ "multi-user.target" ];
      after = [ "systemd-modules-load.service" "systemd-udev-settle.service" ];
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
        ExecStart = "${rdmaUp}/bin/ernic-rdma-up";
        StandardOutput = "journal+console";
        StandardError = "journal+console";
      };
    };
    environment.systemPackages = [ pkgs.rdma-core pkgs.ethtool pkgs.iproute2 pkgs.pciutils ];
  };
}
