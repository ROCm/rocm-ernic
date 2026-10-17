# nix/ionic-pair.nix
#
# Two NixOS guests, each with its own emulated ionic NIC, joined by the
# rocm-ernic TCP backend: node 1's server is the mesh manager, node 2's
# server a worker connecting to it on 127.0.0.1. RDMA traffic between the
# guests goes through the two servers; nothing needs root, /dev/vfio, a
# TAP or RDMA hardware.
#
# The emulated NIC carries no Ethernet without a host TAP, so each guest
# also gets a second virtio NIC ("pair0") on an unprivileged QEMU
# -netdev dgram unix-socket pair. That plain IP link (10.77.0.1/2) is
# what verbs tools use for their out-of-band QPN/GID exchange.
#
# Each server advertises its guest's default RoCE GID (the link-local
# address derived from the NIC MAC) in ERNIC_TCP_GUEST_GIDS, which is how
# the backend maps a destination GID to a mesh node.
#
# Exports:
#   run-ionic-pair-test   boots both guests and runs ibv_rc_pingpong
#                         between them
#   ionic-vm-pair         interactive: both guests up, a root shell on
#                         node 1's console

{ pkgs, lib, nixpkgs, ernicRun, guestModule }:

let
  # Fixed per-node identity. The emulated NIC MACs follow the server's
  # 72:6f:63:6d ("rocm") default prefix.
  nodes = {
    "1" = { mac = "72:6f:63:6d:00:01"; gid = "fe80::706f:63ff:fe6d:1"; pairMac = "52:54:00:77:00:01"; };
    "2" = { mac = "72:6f:63:6d:00:02"; gid = "fe80::706f:63ff:fe6d:2"; pairMac = "52:54:00:77:00:02"; };
  };
  pairIp = i: "10.77.0.${i}";
  peerOf = i: if i == "1" then "2" else "1";

  # Wait for the RDMA port, then print the device, its GID 0 and the port
  # state. Shared by the test and the interactive login message.
  rdmaReady = pkgs.writeShellApplication {
    name = "ernic-pair-rdma-ready";
    runtimeInputs = with pkgs; [ coreutils gawk gnugrep iproute2 rdma-core ];
    text = ''
      for _ in $(seq 1 60); do
        rdma link show | grep -q 'state ACTIVE' && break
        sleep 1
      done
      rdma link show | grep -q 'state ACTIVE'
      dev=$(ibv_devices | awk 'NR > 2 { print $1; exit }')
      [ -n "$dev" ]
      echo "$dev"
    '';
  };

  pairNet = i: pkgs.writeShellApplication {
    name = "ernic-pair-net";
    runtimeInputs = with pkgs; [ coreutils iproute2 ];
    text = ''
      for _ in $(seq 1 30); do
        for path in /sys/class/net/*; do
          if [ "$(cat "$path/address")" = ${nodes.${i}.pairMac} ]; then
            ifname=''${path##*/}
            ip link set "$ifname" name pair0
            ip addr add ${pairIp i}/24 dev pair0
            ip link set pair0 up
            echo "ernic-pair-net: $ifname -> pair0 ${pairIp i}/24"
            exit 0
          fi
        done
        sleep 1
      done
      echo 'ernic-pair-net: no NIC with MAC ${nodes.${i}.pairMac}' >&2
      exit 1
    '';
  };

  # The emulated NIC has no Ethernet path without a host TAP, so IPv6
  # neighbour discovery on the ionic netdev never completes. For a RoCE v2
  # GID, ib_core resolves the destination MAC through the neighbour table,
  # even for a link-local GID, and modify-QP-to-RTR fails without it. Both
  # MACs are fixed, so a permanent entry for the peer's GID replaces neighbour discovery.
  pairNeigh = i: pkgs.writeShellApplication {
    name = "ernic-pair-neigh";
    runtimeInputs = with pkgs; [ coreutils iproute2 ];
    text = ''
      netdev=""
      for path in /sys/bus/pci/drivers/ionic/*/net/*; do
        [ -e "$path" ] && netdev=''${path##*/} && break
      done
      if [ -z "$netdev" ]; then
        echo 'ernic-pair-neigh: no ionic netdev' >&2
        exit 1
      fi
      ip -6 neigh replace ${nodes.${peerOf i}.gid} lladdr ${nodes.${peerOf i}.mac} dev "$netdev" nud permanent
      echo "ernic-pair-neigh: ${nodes.${peerOf i}.gid} -> ${nodes.${peerOf i}.mac} on $netdev"
    '';
  };

  mkNode = { i, extraModule }:
    (nixpkgs.lib.nixosSystem {
      system = pkgs.stdenv.hostPlatform.system;
      modules = [
        "${nixpkgs}/nixos/modules/virtualisation/qemu-vm.nix"
        guestModule
        extraModule
        {
          nixpkgs.pkgs = pkgs;
          system.stateVersion = "26.05";
          networking.hostName = "ernic-node${i}";
          networking.hosts.${pairIp (peerOf i)} = [ "ernic-node${peerOf i}" ];
          boot.kernelPackages = pkgs.linuxPackages_latest;
          virtualisation = {
            ernic.enable = true;
            ernic.socket = "\${ERNIC_PAIR_DIR:?}/ernic-${i}.sock";
            graphics = false;
            memorySize = 4096;
            cores = 4;
            diskImage = null;
            qemu.options = [
              "-netdev dgram,id=pair,local.type=unix,local.path=\"$ERNIC_PAIR_DIR\"/pair-${i}.sock,remote.type=unix,remote.path=\"$ERNIC_PAIR_DIR\"/pair-${peerOf i}.sock"
              "-device virtio-net-pci,netdev=pair,mac=${nodes.${i}.pairMac}"
            ];
          };
          # The pair NIC is found by MAC, next to eth0 (user net, when
          # present) and the ionic netdev, and addressed directly; dhcpcd
          # is kept off it.
          networking.dhcpcd.denyInterfaces = [ "pair*" ];
          networking.firewall.extraCommands = "iptables -A nixos-fw -s 10.77.0.0/24 -j nixos-fw-accept";
          systemd.services.ernic-pair-net = {
            description = "Address the virtio NIC that links the two guests";
            wantedBy = [ "multi-user.target" ];
            before = [ "network-online.target" ];
            wants = [ "systemd-udev-settle.service" ];
            after = [ "systemd-udev-settle.service" ];
            serviceConfig = {
              Type = "oneshot";
              RemainAfterExit = true;
              ExecStart = "${pairNet i}/bin/ernic-pair-net";
            };
          };
          systemd.services.ernic-pair-neigh = {
            description = "Pin the peer's RoCE GID neighbour on the ionic netdev";
            wantedBy = [ "multi-user.target" ];
            requires = [ "ernic-rdma-up.service" ];
            after = [ "ernic-rdma-up.service" ];
            serviceConfig = {
              Type = "oneshot";
              RemainAfterExit = true;
              ExecStart = "${pairNeigh i}/bin/ernic-pair-neigh";
            };
          };
          environment.systemPackages = [ rdmaReady ];
        }
      ];
    }).config.system.build.vm;

  # Host side: ernic-pair-servers PORT CMD... starts the two servers
  # (manager, then a worker connecting to it on PORT) and runs CMD with
  # both sockets up. Sockets and server logs live in ERNIC_PAIR_DIR.
  pairServers = pkgs.writeShellApplication {
    name = "ernic-pair-servers";
    runtimeInputs = with pkgs; [ coreutils ];
    text = ''
      tcp_port=$1
      shift
      exec env ERNIC_TCP_GUEST_GIDS=${nodes."1".gid} \
        ${ernicRun}/bin/ernic-run --socket "$ERNIC_PAIR_DIR/ernic-1.sock" \
          --backend "tcp:manager:listen:$tcp_port" --mac ${nodes."1".mac} \
          --log-file "$ERNIC_PAIR_DIR/ernic-1.log" -- \
        env ERNIC_TCP_GUEST_GIDS=${nodes."2".gid} \
        ${ernicRun}/bin/ernic-run --socket "$ERNIC_PAIR_DIR/ernic-2.sock" \
          --backend "tcp:worker:127.0.0.1:$tcp_port" --mac ${nodes."2".mac} \
          --log-file "$ERNIC_PAIR_DIR/ernic-2.log" -- \
        "$@"
    '';
  };


  ############################################################ test

  testGuest = i:
    let
      role = if i == "1" then ''
        # Client: the server on node 2 may not be listening yet.
        for _ in $(seq 1 60); do
          if ibv_rc_pingpong -d "$dev" -g 0 -n 1000 ${pairIp "2"} > /tmp/pingpong.log 2>&1; then
            ok=1; break
          fi
          sleep 2
        done
        cat /tmp/pingpong.log
        [ "''${ok:-0}" -eq 1 ]
      '' else ''
        timeout 180 ibv_rc_pingpong -d "$dev" -g 0 -n 1000 | tee /tmp/pingpong.log
      '';
    in
    pkgs.writeShellApplication {
      name = "ernic-pair-check";
      runtimeInputs = with pkgs; [ coreutils gnugrep iproute2 iputils rdma-core systemd rdmaReady ];
      text = ''
        trap 'echo ERNIC_PAIR_FAIL; ip -br link; ip -br addr; journalctl -b -u ernic-pair-net --no-pager; dmesg | tail -20; systemctl poweroff --no-block' ERR
        systemctl is-active --quiet ernic-rdma-up.service
        dev=$(ernic-pair-rdma-ready)
        echo "ERNIC_PAIR_PORT_ACTIVE_PASS $dev gid0=$(cat "/sys/class/infiniband/$dev/ports/1/gids/0")"
        for _ in $(seq 1 30); do
          ping -c1 -W1 ${pairIp (peerOf i)} > /dev/null && break
          sleep 1
        done
        ping -c1 -W1 ${pairIp (peerOf i)} > /dev/null
        echo ERNIC_PAIR_LINK_PASS
        ibv_devinfo -v -d "$dev" | grep -E 'GID\[' || true
        ${role}
        grep -q 'iters in' /tmp/pingpong.log
        echo ERNIC_PAIR_PINGPONG_PASS
        echo ERNIC_PAIR_PASS
        systemctl poweroff --no-block
      '';
    };

  testVm = i: mkNode {
    inherit i;
    extraModule = {
      networking.useDHCP = false;
      virtualisation.qemu.networkingOptions = lib.mkForce [ ];
      virtualisation.qemu.options = [ "-no-reboot" ];
      systemd.services.ernic-pair-check = {
        wantedBy = [ "multi-user.target" ];
        wants = [ "ernic-pair-neigh.service" "ernic-pair-net.service" ];
        after = [ "ernic-pair-neigh.service" "ernic-pair-net.service" ];
        serviceConfig = {
          Type = "oneshot";
          ExecStart = "${testGuest i}/bin/ernic-pair-check";
          StandardOutput = "journal+console";
          StandardError = "journal+console";
        };
      };
    };
  };

  testVms = import ./pair-vms.nix {
    inherit pkgs;
    vm1 = "${testVm "1"}/bin/run-ernic-node1-vm";
    vm2 = "${testVm "2"}/bin/run-ernic-node2-vm";
    waitBoth = true;
  };

  run-ionic-pair-test = pkgs.writeShellApplication {
    name = "run-ionic-pair-test";
    runtimeInputs = with pkgs; [ coreutils gnugrep ];
    text = ''
      case "''${1:---accel=auto}" in
        --accel=auto) export QEMU_OPTS='-machine accel=kvm:tcg' ;;
        --accel=kvm) export QEMU_OPTS='-machine accel=kvm' ;;
        --accel=tcg) export QEMU_OPTS='-machine accel=tcg' ;;
        *) echo 'usage: run-ionic-pair-test [--accel=auto|kvm|tcg]' >&2; exit 2 ;;
      esac
      ERNIC_PAIR_DIR=$(mktemp -d -t ernic-pair-test-XXXXXX)
      export ERNIC_PAIR_DIR
      echo "Evidence: $ERNIC_PAIR_DIR"
      cd "$ERNIC_PAIR_DIR"
      unset QEMU_KERNEL_PARAMS QEMU_NET_OPTS NIX_DISK_IMAGE
      tcp_port=$(( 20000 + RANDOM % 20000 ))
      rc=0
      timeout --kill-after=10s 20m ${pairServers}/bin/ernic-pair-servers "$tcp_port" ${testVms}/bin/ernic-pair-test-vms || rc=$?
      fail=0
      for i in 1 2; do
        echo "=== node $i serial"
        grep -E 'ERNIC_PAIR|ernic-rdma-up|iters in|bytes in|rc_pingpong|Couldn' "serial-$i.log" || true
        for sentinel in PORT_ACTIVE_PASS LINK_PASS PINGPONG_PASS PASS; do
          grep -q "ERNIC_PAIR_$sentinel" "serial-$i.log" || { echo "FAIL: node $i missing ERNIC_PAIR_$sentinel" >&2; fail=1; }
        done
        if grep -q ERNIC_PAIR_FAIL "serial-$i.log"; then
          echo "FAIL: node $i check failed" >&2; fail=1
        fi
      done
      if [ "$rc" -ne 0 ] || [ "$fail" -ne 0 ]; then
        echo "FAIL: launcher exited $rc; evidence $ERNIC_PAIR_DIR" >&2
        exit 1
      fi
      echo "PASS: RDMA RC ping-pong between two ionic guests over the TCP backend; evidence $ERNIC_PAIR_DIR"
    '';
  };

  ############################################################ interactive

  # ernic-pair-pingpong [ITERS]: ibv_rc_pingpong server on the peer (over
  # ssh on the pair link), client here.
  pingpong = i: pkgs.writeShellApplication {
    name = "ernic-pair-pingpong";
    runtimeInputs = with pkgs; [ coreutils openssh rdma-core rdmaReady ];
    text = ''
      iters=''${1:-1000}
      case "$iters" in
        "" | *[!0-9]*) echo 'usage: ernic-pair-pingpong [ITERS]' >&2; exit 2 ;;
      esac
      dev=$(ernic-pair-rdma-ready)
      echo "ernic-pair-pingpong: server on ernic-node${peerOf i}, client on $dev here, $iters iterations"
      ssh -n root@ernic-node${peerOf i} "timeout 60 ibv_rc_pingpong -d \$(ernic-pair-rdma-ready) -g 0 -n $iters" &
      server=$!
      ok=0
      for _ in $(seq 1 10); do
        sleep 1
        if ibv_rc_pingpong -d "$dev" -g 0 -n "$iters" ${pairIp (peerOf i)}; then
          ok=1
          break
        fi
      done
      wait "$server" || true
      [ "$ok" -eq 1 ]
    '';
  };

  motd = i: ''
    rocm-ernic pair: this is ernic-node${i}; ernic-node${peerOf i} is the other guest. Each has an
    emulated ionic NIC, and the two rocm-ernic servers are joined by the TCP backend.
      ernic-pair-pingpong [ITERS]  RDMA RC ping-pong with ernic-node${peerOf i} (ibv_rc_pingpong)
      ssh ernic-node${peerOf i}             root shell on the other guest (pair link ${pairIp (peerOf i)})
      rdma link / ibv_devinfo -v   the ionic RDMA device and its GIDs
      ethtool -i <netdev>          driver: ionic; the netdev is in `rdma link`
    Quit: Ctrl-a x, or poweroff. Both guests stop.
  '';

  # Root ssh between the guests uses a per-run key pair the launcher hands
  # to both through the qemu-vm.nix shared directory (/tmp/shared).
  installKeys = pkgs.writeShellApplication {
    name = "ernic-pair-ssh-keys";
    runtimeInputs = with pkgs; [ coreutils ];
    text = ''
      d=/tmp/shared
      install -d -m 0700 /root/.ssh
      if [ -s "$d/authorized_keys" ]; then
        install -m 0600 "$d/authorized_keys" /root/.ssh/authorized_keys
      fi
      if [ -s "$d/id_ed25519" ]; then
        install -m 0600 "$d/id_ed25519" /root/.ssh/id_ed25519
        install -m 0644 "$d/id_ed25519.pub" /root/.ssh/id_ed25519.pub
      fi
    '';
  };

  vmNode = i: mkNode {
    inherit i;
    extraModule = {
      services.getty.autologinUser = "root";
      users.motd = motd i;
      # Key-only root login: the per-run pair key, plus the host user's
      # keys through the 127.0.0.1 forward with --ssh-port.
      services.openssh = {
        enable = true;
        settings = {
          PasswordAuthentication = false;
          KbdInteractiveAuthentication = false;
          PermitRootLogin = "prohibit-password";
        };
      };
      programs.ssh.extraConfig = ''
        Host ernic-node*
          StrictHostKeyChecking accept-new
      '';
      systemd.services.ernic-pair-ssh-keys = {
        wantedBy = [ "multi-user.target" ];
        before = [ "sshd.service" ];
        after = [ "local-fs.target" ];
        serviceConfig = {
          Type = "oneshot";
          ExecStart = "${installKeys}/bin/ernic-pair-ssh-keys";
        };
      };
      environment.systemPackages = [ (pingpong i) ];
    };
  };

  # Node 2 in the background (console to serial-2.log), node 1 on this
  # terminal; when node 1 stops, node 2 is stopped too. $1 is the
  # optional ssh base port.
  pairVms = import ./pair-vms.nix {
    inherit pkgs;
    vm1 = "${vmNode "1"}/bin/run-ernic-node1-vm";
    vm2 = "${vmNode "2"}/bin/run-ernic-node2-vm";
  };

  ionic-vm-pair = pkgs.writeShellApplication {
    name = "ionic-vm-pair";
    runtimeInputs = with pkgs; [ coreutils openssh ];
    text = builtins.readFile ./pair-port.sh + ''
      usage() {
        echo 'usage: ionic-vm-pair [--accel=auto|kvm|tcg] [--ssh-port PORT]  (node 1 on PORT, node 2 on PORT+1)' >&2
        exit 2
      }
      accel='-machine accel=kvm:tcg'
      ssh_port=""
      while [ "$#" -gt 0 ]; do
        case "$1" in
          --accel=auto) accel='-machine accel=kvm:tcg'; shift ;;
          --accel=kvm) accel='-machine accel=kvm'; shift ;;
          --accel=tcg) accel='-machine accel=tcg'; shift ;;
          --ssh-port) [ "$#" -ge 2 ] && [ -n "$2" ] || usage; ssh_port=$2; shift 2 ;;
          *) usage ;;
        esac
      done
      if [ -n "$ssh_port" ]; then ssh_port=$(normalize_pair_port "$ssh_port"); fi

      ERNIC_PAIR_DIR=$(mktemp -d -t ernic-vm-pair-XXXXXX)
      export ERNIC_PAIR_DIR
      cd "$ERNIC_PAIR_DIR"
      export QEMU_OPTS="$accel"
      unset QEMU_KERNEL_PARAMS QEMU_NET_OPTS NIX_DISK_IMAGE SHARED_DIR

      ssh-keygen -q -t ed25519 -N "" -C ernic-pair -f "$ERNIC_PAIR_DIR/id_ed25519"
      for i in 1 2; do
        shared="$ERNIC_PAIR_DIR/node$i/shared"
        mkdir -p "$shared"
        cp "$ERNIC_PAIR_DIR/id_ed25519" "$ERNIC_PAIR_DIR/id_ed25519.pub" "$shared/"
        cat "$ERNIC_PAIR_DIR/id_ed25519.pub" > "$shared/authorized_keys"
        if [ -n "$ssh_port" ]; then
          for key in "$HOME"/.ssh/*.pub; do
            if [ -f "$key" ]; then cat "$key" >> "$shared/authorized_keys"; fi
          done
        fi
      done

      tcp_port=$(( 20000 + RANDOM % 20000 ))
      echo "ionic-vm-pair: run dir $ERNIC_PAIR_DIR (server logs ernic-1.log/ernic-2.log, node 2 console serial-2.log)"
      if [ -n "$ssh_port" ]; then
        echo "ionic-vm-pair: ssh -p $ssh_port root@127.0.0.1 (node 1), ssh -p $(( ssh_port + 1 )) root@127.0.0.1 (node 2)"
      fi
      echo 'ionic-vm-pair: this console is node 1; try ernic-pair-pingpong. Quit with Ctrl-a x'
      exec ${pairServers}/bin/ernic-pair-servers "$tcp_port" ${pairVms}/bin/ernic-pair-vms "$ssh_port"
    '';
  };
in
{
  inherit run-ionic-pair-test ionic-vm-pair;
}
