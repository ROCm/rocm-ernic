# nix/ionic-vm.nix
#
# packages.ionic-vm: the one-command way to try the emulated NIC. It boots
# an interactive NixOS qemu-vm.nix guest with nixosModules.ernic-guest
# against a rocm-ernic server, and drops you at a root shell on the serial
# console with the in-tree ionic/ionic_rdma drivers bound and the RDMA port
# ACTIVE:
#
#   nix run github:ROCm/rocm-ernic#ionic-vm
#   nix run .#ionic-vm -- --ssh-port 2222 --backend loopback
#
# Quit with Ctrl-a x (or poweroff in the guest). Needs only a host where
# QEMU can run (/dev/kvm for speed); no root, no /dev/vfio and no RDMA
# hardware. The guest root is a tmpfs, so every run starts clean.
#
# With --ssh-port, the guest's sshd is forwarded to 127.0.0.1:PORT and the
# host user's ~/.ssh/*.pub keys are handed to the guest through the
# qemu-vm.nix shared directory (SHARED_DIR, /tmp/shared in the guest), so
# `ssh -p PORT root@127.0.0.1` works without a password.

{ pkgs, nixpkgs, ernicRun, guestModule }:

let
  tryCommands = ''
    rocm-ernic emulated AMD Pensando ionic NIC (vfio-user). Things to try:
      rdma link                  RDMA port state (ACTIVE once ernic-rdma-up ran)
      ibv_devinfo -v             verbs view of the ionic RDMA device
      ethtool -i <netdev>        driver: ionic; the netdev is in `rdma link`
      ip -d link                 the ionic netdev next to the virtio one
      lspci -nn -d 1dd8:         the emulated device (1dd8:1002)
      journalctl -u ernic-rdma-up
    Quit: Ctrl-a x, or poweroff.
  '';

  installKeys = pkgs.writeShellApplication {
    name = "ernic-vm-ssh-keys";
    runtimeInputs = with pkgs; [ coreutils ];
    text = ''
      keys=/tmp/shared/authorized_keys
      [ -s "$keys" ] || exit 0
      install -d -m 0700 /root/.ssh
      install -m 0600 "$keys" /root/.ssh/authorized_keys
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
          networking.hostName = "ernic-vm";
          boot.kernelPackages = pkgs.linuxPackages_latest;
          virtualisation = {
            ernic.enable = true;
            graphics = false;
            memorySize = 4096;
            cores = 4;
            diskImage = null;
          };

          services.getty.autologinUser = "root";
          users.motd = tryCommands;

          # Key-only root login, reachable only through the host-side
          # 127.0.0.1 forward the launcher sets up with --ssh-port.
          services.openssh = {
            enable = true;
            settings = {
              PasswordAuthentication = false;
              KbdInteractiveAuthentication = false;
              PermitRootLogin = "prohibit-password";
            };
          };
          systemd.services.ernic-vm-ssh-keys = {
            wantedBy = [ "multi-user.target" ];
            before = [ "sshd.service" ];
            after = [ "local-fs.target" ];
            serviceConfig = {
              Type = "oneshot";
              ExecStart = "${installKeys}/bin/ernic-vm-ssh-keys";
            };
          };
        }
      ];
    }).config.system.build.vm;
in
pkgs.writeShellApplication {
  name = "ionic-vm";
  runtimeInputs = with pkgs; [ coreutils ];
  text = ''
    usage() {
      echo 'usage: ionic-vm [--accel=auto|kvm|tcg] [--ssh-port PORT] [--backend SPEC]' >&2
      exit 2
    }
    accel='-machine accel=kvm:tcg'
    ssh_port=""
    backend=()
    while [ "$#" -gt 0 ]; do
      case "$1" in
        --accel=auto) accel='-machine accel=kvm:tcg'; shift ;;
        --accel=kvm) accel='-machine accel=kvm'; shift ;;
        --accel=tcg) accel='-machine accel=tcg'; shift ;;
        --ssh-port) [ "$#" -ge 2 ] || usage; ssh_port=$2; shift 2 ;;
        --backend) [ "$#" -ge 2 ] || usage; backend=(--backend "$2"); shift 2 ;;
        *) usage ;;
      esac
    done
    case "$ssh_port" in
      "") ;;
      *[!0-9]*) echo "ionic-vm: --ssh-port needs a number, got '$ssh_port'" >&2; exit 2 ;;
      *) if [ "$ssh_port" -lt 1 ] || [ "$ssh_port" -gt 65535 ]; then
           echo "ionic-vm: --ssh-port $ssh_port is out of range" >&2; exit 2
         fi ;;
    esac

    run_dir=$(mktemp -d -t ernic-vm-XXXXXX)
    cd "$run_dir"
    export TMPDIR="$run_dir"
    export QEMU_OPTS="$accel"
    unset QEMU_KERNEL_PARAMS QEMU_NET_OPTS NIX_DISK_IMAGE SHARED_DIR
    # qemu-vm.nix makes its own nix-vm.* TMPDIR; SHARED_DIR is the hand-off.
    export SHARED_DIR="$run_dir/shared"
    mkdir -p "$SHARED_DIR"
    if [ -n "$ssh_port" ]; then
      export QEMU_NET_OPTS="hostfwd=tcp:127.0.0.1:$ssh_port-:22"
      for key in "$HOME"/.ssh/*.pub; do
        if [ -f "$key" ]; then cat "$key" >> "$SHARED_DIR/authorized_keys"; fi
      done
    fi

    echo "ionic-vm: run dir $run_dir (rocm-ernic log: $run_dir/ernic.log)"
    if [ -n "$ssh_port" ]; then
      echo "ionic-vm: ssh -p $ssh_port root@127.0.0.1 once the guest is up"
    fi
    echo 'ionic-vm: quit with Ctrl-a x'
    exec ${ernicRun}/bin/ernic-run --socket "$run_dir/ernic.sock" \
      --log-file "$run_dir/ernic.log" "''${backend[@]}" -- \
      ${vm}/bin/run-ernic-vm-vm
  '';
}
