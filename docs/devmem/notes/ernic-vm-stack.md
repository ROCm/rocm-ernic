# ERNIC two-VM stack for ionic devmem-TX PoC: findings

Paths: `E` = ~/Projects/rocm-ernic (develop), `P` = /var/tmp/ionic-devmem/rocm-ernic-pin (61b934a3),
`K` = /var/tmp/ionic-devmem/kernel-tools, `Q` = ~/Projects/qemu-minimal.
Each claim is tagged VERIFIED (read in source / probed on host) or INFERRED (from kernel knowledge; the linux
clone at /var/tmp/ionic-devmem/linux had no commits when probed, so no kernel source was read).

## 0. Decisive facts / blockers (read first)

- B1 VERIFIED. The emulator advertises **no LIF offload features**: LIF identity `features` left zero
  (E/src/ionic/ionic_eth_emu.c:1142 "features is at 0x0a8; left zero, no offloads are emulated"), and
  LIF_SETATTR only handles the MAC attr (ionic_eth_emu.c:1529-1545), so the FEATURES completion is zero.
  Same at the pin (P/src/ionic/ionic_eth_emu.c:927, :1623). INFERRED consequence: ionic computes
  `hw_features = requested & comp.features = 0`, so the netdev has no NETIF_F_SG. devmem TX needs SG
  (unreadable net_iov frags cannot be linearized; MSG_ZEROCOPY is downgraded without SG). The emulator's TX
  gather already handles SG (nsge up to TXQ_SG_MAX_ELEMS=8, ionic_eth_emu_priv.h:362-363, ionic_eth_emu.c:1947-2000),
  so either the emulator must return IONIC_ETH_HW_TX_SG from LIF_SETATTR(FEATURES), or the PoC driver must force
  SG. The bind (netlink bind-tx) can still be tested without SG; the data path cannot. Needs confirmation in guest
  with `ethtool -k`.
- B2 VERIFIED. System QEMU (/usr/bin/qemu-system-x86_64 8.2.2) has no `vfio-user-pci`. Use
  /opt/qemu-10.2.2-pci-mmio-bridge-submit/bin/qemu-system-x86_64 (10.2.2, has vfio-user-pci), also
  /opt/qemu/bin (10.1.3) and E/../qemu/build (10.2.2).
- B3 VERIFIED. Guest-to-guest Ethernet does NOT go over the TCP mesh; it needs one host TAP per server on a
  shared bridge (E/.github/workflows/ernic-system-tests.yml:1562-1567, :1901-1911). The host has no ernicbr0 or
  ernic-tap* (ip link probe), so the already-running service instances (pids 2991/3704, /run/rocm-ernic/{1,2}.sock,
  no --tap) have dead Ethernet. Creating bridge+taps needs root (sudo -n works on this host).
- B4 VERIFIED. Host ports 5000 and 6320 are in use (6320 by the rocm-ernic.service manager); pick another mesh port.
- B5 VERIFIED. No guest image with kernel >= the targets exists locally; local images are Ubuntu resolute with
  7.2.3/7.2.4 (vm-info.json), user and password as `vm-info.json` records them, key id_rsa beside the image. Custom kernels
  must be installed (deb) or passed with -kernel.
- B6 VERIFIED. The ERNIC bundle kernel config (K/config-scripts/config-ernic-bundle:1-15) is defconfig + amdgpu +
  infiniband + VFIO + IONIC=m + DEBUG_INFO_NONE; it sets no UDMABUF, KASAN or lockdep. K/useful-configs/
  config-x86_64-p2pdma-qemu has KASAN=y but UDMABUF, IONIC, INFINIBAND, PROVE_LOCKING, VIRTIO_FS off.
- B7 VERIFIED. The SRQ series is rebased for v7.2.x only (E/ansible/files/patches/srq/README.md "The base: v7.2.6");
  it was derived from rdma/for-next via v7.3-rc6. INFERRED: do not use the DKMS flow for v7.4-rc; build ionic in-tree.

## 1. How the ionic DKMS module is built

### Sources (VERIFIED)
- Pin: `IONIC_KERNEL_REF "v7.2.6"` (E/cmake/ErnicKernelModule.cmake:31; pin P/cmake/ErnicKernelModule.cmake:34),
  repo stable/linux.git (:36-39). Bundle pins: kernel_tag v7.2.15, ernic_source_pin 61b934a3..., ionic_kernel_pin
  v7.2.6 (K/ernic-pins.yml). So the bundle runs a v7.2.15 kernel with ionic sources from v7.2.6.
- Sparse checkout of exactly three dirs (E/ansible/files/scripts/fetch-ionic-sources.sh:213-216):
  ```
  git sparse-checkout set \
      'drivers/net/ethernet/pensando/ionic' \
      'drivers/infiniband/hw/ionic' \
      'include/uapi/rdma'
  ```
  fetched with `git fetch --depth 1 --filter=blob:none origin $KERNEL_REF` (:235), then `git checkout --detach` (:253).
- Patches (fetch-ionic-sources.sh:261-270): for each `--patches-dir` in the order given, every `*.patch` in glob
  (lexical) order, non-recursive:
  ```
  for d in "${PATCHES_DIRS[@]}"; do
      for p in "${d}"/*.patch; do
          git -c commit.gpgsign=false ... am --whitespace=fix "${p}"
  ```
  Default when no --patches-dir: `${SCRIPT_DIR}/../patches` (:90-92); naming any dir replaces the default (:14-19).
  Relative paths are made absolute (:113-120). Sentinel `.patches-applied-<ref>-<digest of repo+dir names+patch
  contents>` (:133-148) makes reruns no-ops unless a patch changes.

### Patch sets (VERIFIED, identical file lists at pin and develop)
- `patches/` top level: only `0002-ionic-allocate-an-address-handle-for-UC-queue-pairs.patch` (RDMA controlpath).
  There is no 0001 / device-ID patch: the emulator presents 1dd8:1002 subsystem 1dd8:5400
  (E/src/rocm_ernic_server.c:66-69), which stock ionic binds.
- `patches/srq/0001..0009` (RDMA; only 0004 touches eth: ionic_if.h; 0009 is the same fix as top-level 0002).
- `patches/p4/0001-ionic-register-a-P4-auxiliary-device.patch` (eth: ionic_aux.c, ionic_if.h, ionic_lif.h).
- No existing patch touches ionic_txrx.c, ionic_lif.c or ionic_dev.*.

### Who applies what (VERIFIED)
- CMake `fetch-ionic-sources` target: `--patches-dir ansible/files/patches` only, i.e. just 0002
  (ErnicKernelModule.cmake:75-80). In-VM installer: same, from the driver pack (E/service/vm-driver-install.sh.in:72-75).
- Ansible ernic_guest_setup: `--patches-dir <dir>/patches` then each of `ernic_guest_ionic_extra_patches`
  (E/ansible/roles/ernic_guest_setup/tasks/driver_ionic.yml:119-123).
- ERNIC bundle: `--patches-dir srq`, then `--patches-dir p4` if present; NOT the top-level dir
  (K/ktool/ktool/build_ernic_bundle.py:293-306). Order: srq 0001-0009, then p4 0001.

### DKMS staging/build (VERIFIED)
- setup-ionic-dkms.sh: PKG `ionic-ernic`, version = ref minus leading v (:32, :64-65); tree /usr/src/ionic-ernic-7.2.6
  with `eth/` (copy of drivers/net/ethernet/pensando/ionic), `rdma/`, `uapi/rdma/ionic-abi.h` (:67-75, :106-115);
  Kbuild builds ionic.ko from eth/*.c and ionic_rdma.ko from rdma/*.c (:118-139); AUTOINSTALL=no (:175);
  `dkms add`, `dkms build -k $(uname -r)`, `dkms install` -> /updates/dkms (:188-243). Every step uses sudo and
  builds for the RUNNING kernel only, so it must run inside the guest.
- The bundle does not run dkms on the host: it writes the same tree + Kbuild + dkms.conf into
  `ionic-ernic-dkms_7.2.6_all.deb` with dpkg-deb, and postinst only does `dkms add` (build_ernic_bundle.py:311-396);
  the target runs `dkms build -m ionic-ernic -v 7.2.6 -k <kver>`.
- ErnicGuestPackage.cmake guest packages are stubs: "install() targets are not yet wired" (E/cmake/ErnicGuestPackage.cmake:100-102, end of file).

### Where a devmem-TX series goes
- INFERRED recommendation: a new directory, e.g. `ansible/files/patches/devmem-tx/000N-*.patch`, passed as the
  LAST `--patches-dir` (after srq and p4), since srq/0004 and p4/0001 already edit ionic_if.h / ionic_lif.h and the
  series must be generated on top of them. For a scratch test with no repo change, keep the series outside the
  repo and run:
  ```
  fetch-ionic-sources.sh --source-dir /var/tmp/ionic-devmem/ionic-src --kernel-ref v7.2.6 \
     --patches-dir $P/ansible/files/patches/srq --patches-dir $P/ansible/files/patches/p4 \
     --patches-dir /var/tmp/ionic-devmem/patches/devmem-tx
  ```
  Dropping it in the top-level `patches/` would only reach the CMake/in-VM/Ansible flows, not the bundle.
- The series must be a backport onto v7.2.6's ionic eth sources (git am, so mbox format with a From/Subject).
  Core netmem TX (dev->netmem_tx) is INFERRED present in 7.2 (landed 6.16).
- Without DKMS (no root on host, any kernel): the same Kbuild works as an external module build against a built
  kernel tree: `make -C <kbuild-dir> M=<staged tree> modules` (the MAKE line, setup-ionic-dkms.sh:177).

## 2. How VMs are booted

### rocm-ernic CI / ernicctl flow: ci/boot-guest.sh (VERIFIED)
- Usage: `--disk FILE --name NAME --ssh-port PORT --out DIR [--vfio-user SOCKET] [--qemu BINARY] ... [-- QEMU ARGS]`
  (E/ci/boot-guest.sh:42-92); args after `--` are appended verbatim (:327-329, :688), so `-kernel/-append/-virtfs`
  can be added without editing anything.
- QEMU command (:666-689):
  ```
  -machine q35,accel=$ACCEL,memory-backend=mem0,hpet=off -cpu $CPU -smp $CPUS -m ${MEM}M
  -object memory-backend-memfd,id=mem0,share=on,size=${MEM}M
  -device pcie-root-port,id=pcie-vfu.1,chassis=N
  -device {"driver":"vfio-user-pci","id":"vfio-user0","bus":"pcie-vfu.1","socket":{"type":"unix","path":"$SOCK"}}
  -drive file=$DISK,format=qcow2,if=virtio  -drive file=$OUT/seed.img,format=raw,if=virtio,readonly=on
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22 -device virtio-net-pci,netdev=net0
  ```
  (:613-620 vfio-user, :627 netdev). "vfio-user needs guest memory the server can map, so it's a shared memfd" (:662).
  No hugepages needed; none configured on host (HugePages_Total 0).
- Credentials: per-boot ed25519 key via NoCloud seed (cloud-localds), image needs cloud-init (:11-17, :481, :531).
  Outputs pid/key/user/port (:21-25). Defaults 2 vCPU, 4096 MiB (:59-60).
- Guest image: ORAS artifact ghcr.io/rocm/rocm-ernic/guest-ionic:resolute-k7.2.6-d858ab11a7ec, disk
  ubuntu-ci-vm.qcow2, user ubuntu, "no password and no key" (E/ci/guest-image.env); fetched by
  `scripts/fetch-guest-image.sh --dest DIR` (E/scripts/fetch-guest-image.sh:27-33). Kernel is installed in the image
  (7.2.6-070206-generic, srq/README.md); no -kernel is used anywhere in this flow. Not present on this host; pulling
  from the ROCm org needs a token with access to that organisation, untested.
- CI two-VM boot call: system-tests.yml:2256-2276 (ssh ports 2222/2223, `--shared-backing`, `--vfio-user /tmp/vfio-user-server$n.sock`).

### qemu-minimal flow (VERIFIED)
- `qemu-tool run-vm --vfio-userdev SOCK[,SOCK] --filesystem DIR --qemu-path PATH --ssh-port ...` (run-vm --help).
  Emits `{"driver":"vfio-user-pci",...,"rombar":0,...}` (Q/qemu/qemu-tool/run_vm.py:428-438),
  `memory-backend-memfd,id=mem-vfio-user,size=..M,share=on` + `-machine q35...,memory-backend=mem-vfio-user`
  (:500-501), and `--filesystem` -> `-virtfs local,path=DIR,security_model=passthrough,mount_tag=hostfs` (:292-294;
  9p, mount_tag hostfs, also gen_compose.py:59-65). No x86 -kernel support (only riscv u-boot, :243).
- Compose stack `vfio-user-ernic-2vm`: two ernic containers (manager hub :6320 / worker), each with a tapsetup
  sidecar (NET_ADMIN) and `rocm-ernic -s /run/vfu/ernic-N.sock ... -T tap0`, plus two qemu containers running
  `qemu-tool run-vm --vfio-userdev` (Q/qemu/compose/vfio-user-ernic-2vm/docker-compose.yml:72-171, :250-256;
  README.md). Uses published docker images of rocm-ernic, so a locally patched server would need ERNIC_IMAGE.
- kernel-tools dmabuf-test: OCI qcow2 via oras + `qemu-tool gen-vm`, `qemu-tool compose --stack vfio-user-rocjitsu-vm up`
  (K/ktool/ktool/dmabuf_test.py:203-257), ssh with images_dir/id_rsa (:98-110). Custom kernel path: copy guest
  /boot/config, localmodconfig, `make deb-pkg` on host (:308-356), scp debs, `dpkg -i`, pin GRUB_DEFAULT, reboot
  (:441-484). This is the existing "custom kernel in guest" recipe; it never uses -kernel.
- Bundle kernel: clone v7.2.15, `patch -p1` 20 dmabuf-72x-backport patches, `make defconfig` + config-ernic-bundle +
  olddefconfig, `make LOCALVERSION=-ernic-bundle deb-pkg` (build_ernic_bundle.py:187-229).

### Local images (VERIFIED by ls / vm-info.json)
- ~/.cache/ernic-guest/output/: batesste-ci-vm.qcow2 (5.6G base) + overlays .1/.2, id_rsa, vm-info.json: flavour
  ionic, user and password as vm-info.json records them, resolute, kernel 7.2.3-070203-generic.
- /opt/qemu-images/artifacts/20260916...ionic...: same shape, 7.2.3 (this is ERNIC_VM_BACKING in
  /etc/rocm-ernic/rocm-ernic.env); 20260925...ernic-rocjitsu...: 7.2.4, 20G.
- Q/images/: resolute-server-cloudimg-amd64.img, noble cloudimg, ernic-test-vm* (some root-owned), many fleet overlays.

## 3. Connecting two guests

- VERIFIED. RDMA path: `--backend tcp:manager:listen:PORT` on server 1, `--backend tcp:worker:HOST:PORT` on server 2
  (system-tests.yml:1920-1926, :2028-2034; server --help). GIDs each node owns are declared by env
  `ERNIC_TCP_GUEST_GIDS="fe80::a0:d1ff:fe00:1,192.168.200.10"`. Node ids are assigned by the mesh, not a CLI option
  (no --node-id in --help). Irrelevant for plain TCP but harmless; `--backend loopback` or `none` also works for
  an Ethernet-only test (INFERRED: Ethernet is independent of backend per system-tests.yml:1562-1564).
- VERIFIED. Ethernet path: `--tap IFNAME` (pre-created, IFF_TAP|IFF_NO_PI, E/src/ionic/ionic_eth_net.c:38-64); both
  taps enslaved to bridge ernicbr0; distinct `--mac` per instance is mandatory, otherwise both guests share the
  default MAC and ARP fails (system-tests.yml:1905-1911). Host commands (E/docs/testing.rst:509-520;
  E/ansible/roles/ernic_host_setup/tasks/tap.yml:61-98):
  ```
  sudo ip link add ernicbr0 type bridge; sudo ip link set ernicbr0 up
  for n in 1 2; do sudo ip tuntap add dev ernic-tap$n mode tap user $USER
                   sudo ip link set ernic-tap$n master ernicbr0 up; done
  ```
  The launcher adds --tap only if /sys/class/net/ernic-tapN exists (E/service/rocm-ernic-launcher:249-253).
- VERIFIED. Guest IP: netdev is `rocm-ernic0` with udev/99-rocm-ernic.rules (:30-31), else enpXsY; guest-setup does
  `ip link set rocm-ernic0 up; ip addr add 192.168.200.{10,20}/24 dev rocm-ernic0`
  (E/ansible/roles/ernic_guest_setup/tasks/nic.yml:54-85; defaults/main.yml:176-188).
- VERIFIED. Existing two-VM TCP automation: yes. `ansible-playbook ci-site.yml --tags sanity` runs ping both ways and
  a netcat echo on :9999 over the emulated Ethernet (E/ansible/playbooks/sanity-tests.yml:84-175);
  `--tags perf` runs iperf3 (E/ansible/playbooks/tcp-performance-tests.yml:121-144), scheduled/dispatch only
  (system-tests.yml:2538-2545). tests/test_tcp_mesh_ci.sh and scripts/test-tcp-local.sh are server-only (no VM).
  None exercises MSG_ZEROCOPY or devmem.
- VERIFIED. Emulator TX: one frame per descriptor, len <= emu->frame, SG up to 8 elems read by DMA from guest
  addresses; no TSO/csum (ionic_eth_emu.c:1931-2000). INFERRED: udmabuf pages are ordinary guest RAM in the shared
  memfd and there is no vIOMMU on the command line, so the dma-buf's DMA addresses are guest-physical and the
  emulator can read them.

## 4. Host readiness (all VERIFIED by read-only probe unless noted)

| Item | State |
| --- | --- |
| QEMU with vfio-user-pci | /usr/bin 8.2.2: NO. /opt/qemu-10.2.2-pci-mmio-bridge-submit/bin, /opt/qemu/bin (10.1.3), ~/Projects/qemu/build (10.2.2): YES |
| libvfio-user | built in ~/Projects/libvfio-user/build, installed /usr/local/lib/x86_64-linux-gnu/libvfio-user.so.0 (no .pc on pkg-config path) |
| rocm-ernic server | ~/Projects/rocm-ernic/build/rocm-ernic (Oct 2, links libvfio-user, has --tap/--mac); /usr/local/bin/rocm-ernic (Sep 25, root-installed, has --tap); only one build dir (`build`) |
| Running service | rocm-ernic.service: 2 instances, mesh port 6320, sockets /run/rocm-ernic/{1,2}.sock (0666), no taps, vm: null |
| KVM | /dev/kvm root:kvm 0660; user is in kvm (also libvirt, docker, rocm-ernic) |
| sudo | `sudo -n true` exit 0: passwordless sudo available |
| /dev/net/tun | 0666; creating taps still needs CAP_NET_ADMIN (sudo); attaching to a tap owned by $USER does not |
| Bridge/taps | none (only virbr0, docker0) |
| Hugepages | 0 configured; not needed (memfd share=on) |
| RAM/CPU/disk | 503G (381G avail), 128 CPUs, /var/tmp 2.9T free |
| Tools | cloud-localds, qemu-img, oras, ansible-playbook, docker, dkms, virtiofsd (/usr/libexec/virtiofsd), qemu-tool (~/.local/bin) present; iperf3 NOT on host PATH |
| Busy ports | 5000, 6320 |
| Kernel tree | /var/tmp/ionic-devmem/linux: git repo with no commits yet |

Sharing a built kernel tree with guests: 9p via `-virtfs local,path=DIR,security_model=none|passthrough,mount_tag=hostfs`
(needs CONFIG_NET_9P_VIRTIO + 9P_FS in guest kernel, or the distro kernel's modules); virtiofs works too since memory
is already shared memfd (needs virtiofsd per guest + CONFIG_VIRTIO_FS). Simplest: scp debs/.ko over the forwarded ssh port.

Guest kernel CONFIG for the test (INFERRED from upstream Kconfig; verify once the tree is cloned):
- devmem: NET_DEVMEM (def_bool y; depends on DMA_SHARED_BUFFER, GENERIC_ALLOCATOR, PAGE_POOL), PAGE_POOL (selected
  by a page-pool NIC driver, e.g. IONIC or VIRTIO_NET), UDMABUF=y (depends on DMA_SHARED_BUFFER, MEMFD_CREATE),
  DMABUF_HEAPS optional.
- driver: NET_VENDOR_PENSANDO, IONIC=m (and INFINIBAND=m + INFINIBAND_IONIC if RDMA module is wanted; ionic_rdma
  from DKMS needs INFINIBAND core + AUXILIARY_BUS).
- boot: VIRTIO_PCI, VIRTIO_BLK, VIRTIO_NET, EXT4_FS, PCI_MSI, HOTPLUG_PCI_PCIE; for -kernel without initrd make
  these =y; NET_9P, NET_9P_VIRTIO, 9P_FS (or VIRTIO_FS) for sharing.
- debug (7.4-rc build): KASAN + KASAN_GENERIC (+KASAN_INLINE), PROVE_LOCKING, DEBUG_LOCKDEP, DEBUG_ATOMIC_SLEEP,
  DEBUG_SPINLOCK, DEBUG_MUTEXES, KFENCE optional, DEBUG_INFO (BTF off to save time), PANIC_ON_OOPS optional,
  FAULT_INJECTION optional. Out-of-tree ionic must be built against the same KASAN/lockdep tree.
- ncdevmem selftest: tools/testing/selftests/drivers/net/hw (ncdevmem.c, devmem.py; needs YNL generated libs,
  `make -C tools/testing/selftests TARGETS=drivers/net/hw`), socat on the peer, IPV6, ethtool. TX mode of ncdevmem
  uses udmabuf + netlink bind-tx + sendmsg(MSG_ZEROCOPY, SCM_DEVMEM_DMABUF); the receiver can be plain socat.

## 5. Shortest path (proposed; nothing here was executed)

Root needed on host: only step 1 (bridge + taps) and, if used, apt installs. Everything else (servers on private
sockets, QEMU with KVM via kvm group, kernel/module builds, scp) runs as the unprivileged login user. Root in guest: dpkg/insmod/ip
(guest sudo; the image's own user on local images, INFERRED passwordless sudo, its password otherwise).

1. Host network (root):
   `sudo ip link add ernicbr0 type bridge && sudo ip link set ernicbr0 up; for n in 1 2; do sudo ip tuntap add dev ernic-tap$n mode tap user $USER; sudo ip link set ernic-tap$n master ernicbr0 up; done`
2. Two private servers (no root; do not touch the service's instances):
   ```
   D=/var/tmp/ionic-devmem/run; mkdir -p $D
   ~/Projects/rocm-ernic/build/rocm-ernic -s $D/e1.sock -b loopback -T ernic-tap1 -m 02:a0:d1:00:00:01 -l $D/e1.log &
   ~/Projects/rocm-ernic/build/rocm-ernic -s $D/e2.sock -b loopback -T ernic-tap2 -m 02:a0:d1:00:00:02 -l $D/e2.log &
   ```
   (or `-b tcp:manager:listen:6330` / `-b tcp:worker:127.0.0.1:6330` with ERNIC_TCP_GUEST_GIDS if RDMA is wanted).
   If B1 requires an emulator change, build it in a scratch copy and point here.
3. Overlays + boot (no root). The local image may lack a usable cloud-init path for boot-guest.sh's per-boot key,
   but it already trusts id_rsa; either pass `--user <image-user> --authorize ~/.cache/ernic-guest/output/id_rsa.pub`
   or run QEMU directly:
   ```
   B=~/.cache/ernic-guest/output/batesste-ci-vm.qcow2
   for n in 1 2; do qemu-img create -f qcow2 -b $B -F qcow2 $D/vm$n.qcow2; done
   ~/Projects/rocm-ernic/ci/boot-guest.sh --qemu /opt/qemu-10.2.2-pci-mmio-bridge-submit/bin/qemu-system-x86_64 \
     --disk $D/vm1.qcow2 --name devmem1 --ssh-port 2261 --out $D/vm1 --user <image-user> --cpus 8 --mem 8192 \
     --shared-backing --vfio-user $D/e1.sock -- -virtfs local,path=/var/tmp/ionic-devmem,security_model=none,mount_tag=hostfs
   ```
   (same for vm2 with port 2262, e2.sock). ssh: `ssh -i <key from output> -p 2261 <image-user>@127.0.0.1`.
4. Kernels (no root on host): in the linux tree, for each of v7.4-rcN and v7.2.15: start from the guest's
   /boot/config-* (dmabuf_test.py recipe) or defconfig + kvm_guest.config, enable the section-4 options, then
   `make -j128 bindeb-pkg LOCALVERSION=-devmem`; scp linux-image/headers debs to both guests, `sudo dpkg -i`, pin
   grub default, reboot (dmabuf_test.py:453-484). For v7.2.15 optionally apply K/patches/dmabuf-72x-backport to
   match the bundle (not needed for TCP devmem).
5. (a) Baseline, unpatched: boot the new kernel with stock in-tree ionic (CONFIG_IONIC=m) or the DKMS tree built
   from srq+p4 only. In each guest: `sudo ip link set <ionic netdev> up; sudo ip addr add 192.168.200.{10,20}/24 dev <netdev>`;
   `ping`, `iperf3`/nc sanity. Then on vm2: `socat -u TCP-LISTEN:5201,reuseaddr - | sha256sum`; on vm1:
   `ncdevmem -s 192.168.200.20 -p 5201 -f <netdev> < file`. Expected (INFERRED): bind-tx netlink fails with
   EOPNOTSUPP ("driver does not support netmem TX") because dev->netmem_tx is false. Also record `ethtool -k <netdev> | grep scatter`.
   (b) Patched: v7.4-rc: apply the series in-tree, `make M=drivers/net/ethernet/pensando/ionic modules`, scp ionic.ko,
   `sudo modprobe -r ionic_rdma ionic; sudo insmod ionic.ko` (or modules_install + modprobe). v7.2.15: run
   fetch-ionic-sources.sh with srq, p4, devmem-tx dirs (section 1), copy the tree into the guest and run
   `setup-ionic-dkms.sh --source-dir ... --kernel-ref v7.2.6` there (needs dkms + headers deb in guest), or build
   on the host with `make -C <7.2.15 build tree> M=<staged tree> modules` and scp the .ko. Re-run ncdevmem; watch
   dmesg for KASAN/lockdep splats and the server log/stats (`-S`) for TX frames/drops.

## Open questions
1. B1: does the emulator's zero feature set leave NETIF_F_SG off in the guest, and if so is patching the emulator
   (LIF_SETATTR FEATURES -> TX_SG) in scope, given "do not modify any repository"?
2. Does the v7.4-rc ionic already contain the SRQ/aux changes, and does the emulator at develop/pin work with the
   unpatched 7.4-rc ionic eth driver (identity layout offsets, srq/README.md)? Untested.
3. Which server build to use: build/rocm-ernic is from Oct 2 and predates current develop; the bundle pins 61b934a3.
   A fresh build of the pin or develop may be wanted (needs -DCMAKE_PREFIX_PATH or the /usr/local libvfio-user).
4. Do the local batesste-ci-vm images have cloud-init, passwordless sudo, dkms, build tools, socat, iperf3? Not probed
   (would require booting).
5. ERNIC MTU/frame limit and absence of TSO: devmem sends will be MSS-sized single frames; fine for a PoC, but
   throughput numbers are meaningless.
6. ghcr.io/rocm/rocm-ernic/guest-ionic (k7.2.6) pull access from this host is untested.

## Verification pass

Independent re-check (2026-11-18). E/P/K/Q as above. Kernel-side claims were checked against
raw.githubusercontent.com/torvalds/linux at `v7.2` and `master` (the local clone is still empty), so they
hold for v7.2 mainline and current master, not specifically for v7.2.15 / a given v7.4-rc. Nothing was
booted or built. Only this section was added.

### Blockers
- B1 emulator advertises no LIF features: CONFIRMED. E/src/ionic/ionic_eth_emu.c:1142 (identity features
  left zero), :1523-1537 (`handle_lif_setattr` handles only `IONIC_LIF_ATTR_MAC`, comp otherwise zero);
  pin P/src/ionic/ionic_eth_emu.c:927, :1265. Guest consequence, previously INFERRED, now CONFIRMED in
  kernel source: v7.2 ionic_lif.c:1524 `lif->hw_features = le64_to_cpu(cmd.features & comp.features)`,
  :1599-1600 NETIF_F_SG only `if (lif->hw_features & IONIC_ETH_HW_TX_SG)`; no unconditional SG.
- B1 "advertising TX_SG looks small": WRONG (incomplete). SG alone is not enough. The stack cannot
  checksum unreadable frags: `skb_checksum_help()` returns -EFAULT when `!skb_frags_readable(skb)`
  (v7.2 net/core/dev.c:3582). So the netdev also needs NETIF_F_HW_CSUM, which ionic sets only from
  `IONIC_ETH_HW_TX_CSUM` (v7.2 ionic_lif.c:1458-1459, :1602-1603), and the driver then posts
  `IONIC_TXQ_DESC_OPCODE_CSUM_PARTIAL` descriptors with csum_start/csum_offset (ionic_txrx.c:1565-1576).
  The emulator's `eth_tx_gather()` ignores the descriptor opcode and never fills a checksum
  (E/src/ionic/ionic_eth_emu.c:1935-2000; comment :1951-1953 "We advertise no offloads"), and the peer's
  RX completion has csum_flags zero so the receiver verifies in software (:2221-2224). The emulator change
  is therefore: return TX_SG and TX_CSUM from LIF_SETATTR(FEATURES), and compute the L4 checksum for
  CSUM_PARTIAL descriptors before sending to the TAP. "PoC driver forces SG" does not avoid this.
- B1 "MSG_ZEROCOPY is downgraded without SG": UNVERIFIABLE as stated. tcp.c keys on `sk->sk_route_caps`
  (v7.2 net/ipv4/tcp.c:1143-1168), which for TCP can carry SG from software GSO independent of the device;
  where exactly a devmem send fails without device SG/CSUM (sendmsg error vs. drop at validate_xmit) was
  not determined. The conclusion (no data path without emulator offloads) stands via dev.c:3582.
- B1 TX gather supports SG up to 8: CONFIRMED (E ionic_eth_emu_priv.h:363, ionic_eth_emu.c:1962-1998;
  Q_IDENTIFY TXQ reports version 0, max_sg_elems 8 at :1612-1616; same at pin :1330-1332, :1618-1663).
  Note `sg_total >= len` is rejected (:1982), so the head fragment must be non-empty (true for
  headers-in-linear devmem skbs).
- NEW (not in notes): `netmem_tx` is not a bool. v7.2 and master: `enum netmem_tx_mode
  {NETMEM_TX_NONE, NETMEM_TX_DMA, NETMEM_TX_NO_DMA}`, `unsigned long netmem_tx:2`
  (v7.2 include/linux/netdevice.h:1805-1808, :2160). The ionic patch sets `NETMEM_TX_DMA`.
  ionic has no netmem reference at master (grep of ionic_lif.c/ionic_txrx.c/ionic_main.c).
- Baseline bind failure EOPNOTSUPP: CONFIRMED on master: `netdev_nl_bind_tx_doit()` returns -EOPNOTSUPP
  "Driver does not support netmem TX" when `netdev->netmem_tx == NETMEM_TX_NONE` (net/core/netdev-genl.c).
  Not re-read at v7.2.15.
- B2 QEMU: CONFIRMED by probe. /usr/bin 8.2.2 has no vfio-user device; /opt/qemu-10.2.2-pci-mmio-bridge-submit,
  /opt/qemu (10.1.3) and ~/Projects/qemu/build (10.2.2) all list `vfio-user-pci`.
- B3 Ethernet via TAP, not mesh: CONFIRMED. system-tests.yml:1562-1567, :1901-1911; the only net backend
  is `ionic_eth_net_open_tap()` (E/src/ionic/ionic_eth_net.h:32). Host has no ernicbr0/ernic-tap*; running
  instances 2991/3704 have no --tap (pgrep). Launcher adds --tap only if the tap exists
  (E/service/rocm-ernic-launcher:249-254).
- B4 ports: CONFIRMED (0.0.0.0:6320, *:5000 listening).
- B5 local images: CONFIRMED (~/.cache/ernic-guest/output/vm-info.json: <image-user> / <its password> / resolute /
  7.2.3, id_rsa + id_rsa.pub present). Guest sudo/cloud-init/packages: UNVERIFIABLE without booting.
- B6 bundle config: CONFIRMED on the missing options, WRONG on the composition: config-ernic-bundle also
  pulls in K/config-scripts/config-default (sourced by config-amdgpu-dkms and config-infiniband because the
  bundle sets SCRIPT_DIR, build_ernic_bundle.py:213), which adds NVMe and NET_9P, NET_9P_VIRTIO, 9P_FS =m.
  So a bundle-config kernel can mount a 9p share. Still no UDMABUF/KASAN/PROVE_LOCKING anywhere in
  config-scripts. p2pdma-qemu config: KASAN=y, UDMABUF/IONIC/INFINIBAND/PROVE_LOCKING/VIRTIO_FS off: CONFIRMED.
- B7 SRQ series based on v7.2.6: CONFIRMED (srq/README.md); "don't use DKMS for 7.4-rc" remains INFERRED.

### DKMS build
- Pins v7.2.15 kernel / 61b934a3 / ionic v7.2.6: CONFIRMED (K/ernic-pins.yml; build_ernic_bundle.py:85-96
  reads them; E cmake/ErnicKernelModule.cmake:31, P :34). Tags v7.2.6, v7.2.15 exist in stable; v7.4-rc3
  is the newest v7.4 tag in torvalds (git ls-remote).
- Sparse checkout of the three dirs, depth-1 fetch, detach: CONFIRMED (fetch-ionic-sources.sh:213-216, :235,
  :253). The pin's script lacks the mirror/retry logic but has the same checkout and apply loop.
- `git am --whitespace=fix`, dirs in order given, `*.patch` in glob order, default dir replaced when any is
  named: CONFIRMED (:14-19, :90-92, :261-270).
- Patch sets and "no patch touches ionic_txrx.c / ionic_lif.c / ionic_dev.*": CONFIRMED (diff --git headers
  of all kernel patches; eth files touched are only ionic_aux.c, ionic_if.h, ionic_lif.h). Lists identical
  at pin and develop.
- Who applies what: CONFIRMED. CMake: top-level dir only (ErnicKernelModule.cmake:50, :79); in-VM installer
  (service/vm-driver-install.sh.in:72-75); Ansible top-level + `ernic_guest_ionic_extra_patches`
  (driver_ionic.yml:115-120; default `[]`, defaults/main.yml:77); bundle srq then p4, not top-level
  (build_ernic_bundle.py:293-306). Omitted by the notes: the bundle also builds a separate ernic-p4 DKMS
  deb from P/drivers/ernic_p4 (:398-401); irrelevant to Ethernet TX.
- Insertion point (new dir as last --patches-dir): CONFIRMED as consistent with the script; still a
  recommendation. The Ansible flow can only take extras that live under the staged patches dir
  (ionic_fetch_controller.yml:27-31).
- setup-ionic-dkms.sh uses sudo throughout and builds for `uname -r`: CONFIRMED (:82-243, :190-192);
  MAKE line :177. Bundle deb built with dpkg-deb, postinst only `dkms add`: CONFIRMED (:311-396).
- "Core netmem TX landed 6.16": not re-checked beyond presence at v7.2 (CONFIRMED present at v7.2).

### Boot
- boot-guest.sh command line, `--` passthrough, memfd share=on, root port + vfio-user-pci, NoCloud seed,
  127.0.0.1 hostfwd: CONFIRMED (E/ci/boot-guest.sh:327-330, :481-531, :613-620, :627, :662-689).
- WRONG by omission: ci/boot-guest.sh does not exist at the pin 61b934a3 (ls P/ci/boot-guest.sh fails).
  Use develop's script; it is independent of the server version.
- CI guest image/env and no `-kernel` on x86 anywhere: CONFIRMED (ci/guest-image.env; run_vm.py:243 riscv
  only). CI builder QEMU is 11.1.1; this host's newest is 10.2.2 (works for vfio-user-pci, untested here).
- qemu-minimal `--filesystem` is 9p, mount tag hostfs: CONFIRMED (Q/qemu/qemu-tool/run_vm.py:289-294).
- kernel-tools custom-kernel recipe (guest /boot/config + localmodconfig + deb-pkg, dpkg -i, pin grub,
  reboot): CONFIRMED (dmabuf_test.py:308-356, :441-484). Caveat the notes missed: localmodconfig with the
  guest's lsmod will drop anything not loaded (UDMABUF etc.), so enable options after it.
- `--user <image-user> --authorize id_rsa.pub` on the local image: UNVERIFIABLE (depends on cloud-init in the
  image and on what it does to an existing user's password/sudo).

### Linking and tests
- `--tap`, distinct `--mac`, bridge commands, rocm-ernic0, .10/.20: CONFIRMED (system-tests.yml:1905-1926,
  :2028-2034; docs/testing.rst:509-520; udev/99-rocm-ernic.rules:25-31; defaults/main.yml:177-188).
  build/rocm-ernic --help lists -T/--tap, -m/--mac, -l, -S; default backend is loopback.
- Existing automation: CONFIRMED with a precision fix: the PR lane (`--tags sanity`) does ping both ways and
  a netcat echo on :9999 (sanity-tests.yml:84-175); iperf3 is only in `--tags perf`
  (tcp-performance-tests.yml:113-144), which is skipped on pull requests (system-tests.yml:2538-2545, :2571).
  The original summary's "already runs ping, netcat and iperf3" overstates the PR lane.
- udmabuf pages readable by the emulator (no vIOMMU): still INFERRED; nothing in source contradicts it.

### Host readiness
- KVM group, `sudo -n true` = 0, /dev/net/tun 0666, no hugepages, tools present, iperf3 absent on host,
  libvfio-user in /usr/local, build/rocm-ernic Oct 2 linking it, single build dir: all CONFIRMED by probe.
- Server build age matters more than the notes say: build/rocm-ernic (Oct 2) predates the pin (committed
  Oct 28) and develop; /usr/local/bin/rocm-ernic is Sep 25. Neither was checked for what Ethernet emulation
  it contains. Since an emulator change is required anyway (B1), build a fresh server.

### Shortest path
- Steps 1-4: CONFIRMED as consistent with source; none executed.
- Step 5(a): CONFIRMED in principle (bind-tx EOPNOTSUPP) on master.
- Step 5(b) "patched module load" is WRONG as a complete test: with the driver patched but the emulator
  unchanged, bind-tx should succeed but no devmem payload can leave the guest (no SG/HW_CSUM). A data-path
  test needs the emulator change described under B1, in a scratch copy or branch of rocm-ernic.
