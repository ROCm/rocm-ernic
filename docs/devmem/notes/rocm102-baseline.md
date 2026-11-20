# Stack 3 (`ionic-devmem-r102`): upstream qemu-tool v2.1.0 + ROCm 10.2 guest, baseline

2026-11-19. Everything here was RUN unless marked **NOT RUN**. Nothing of ours is in the
stack: upstream images, upstream server, upstream guest driver. `B` =
`/var/tmp/ionic-devmem/test-rocm102`, `Q` = `/var/tmp/ionic-devmem/qemu-minimal-2.1`
(tag `v2.1.0`). Study that preceded this: `notes/qemu-tool-2.1-rocm102.md`.

**State as left: stack RUNNING, pristine.** Cold-cycled after the P4 test (so no program in
NIC 1), then one base `quick-start test` (so amdgpu is loaded and the fleet addresses are
set). Stacks 1 and 2 were not touched (still up, same uptimes).

## 1. What it is

| Item | Value |
| --- | --- |
| Compose project / stack dir | `ionic-devmem-r102` / `B/stack/ionic-devmem-r102` |
| Fleet | `Q/qemu/stacks/rocm-golden.xml` + overlay `B/stack/r102-fleet.xml` (names, subnets, port base, images dir; nothing else) |
| Guests | `r102-1`, `r102-2`: 8 vCPU, 8192 MiB (quick-start's own host sizing), 1 ernic NIC, 1 rocjitsu gfx1250 GPU, 1 SDXI, 1 NVMe (1 TiB, `vram-dev=rocjitsu-N`) |
| Guest image | `docker.io/sbates130272/qemu-tool-rocm-golden:20261119.g2aea67d` (digest `sha256:14b3785b...`), overlays in `B/images` (8.9 GB) |
| Other images | qemu fork `20261112.gdca6903-qemu.4cb9698-vfu.3719c3a`, ernic `...-ernic.90434a2-vfu.3719c3a`, rocjitsu `...-rocjitsu.50be05a`, sdxi `20261113.g379fbf6-sdxi.dfd76f2`; all as published |
| SSH | `127.0.0.1:2281` (r102-1), `:2282` (r102-2), user `ubuntu`, key `B/key/id_rsa`; `B/scripts/gssh N` |
| Mesh network | `ionic-devmem-r102_fleet` 172.27.0.0/16 (`<fleet-prefix>172.27`) |
| Guest-Ethernet bridge | `ionic-devmem-r102_l2` **172.28.102.0/24**, pinned in `docker-compose.override.yml` |
| Guest Ethernet addresses | **10.102.100.11 / .12** on `rocm-ernic0` (`<ernic-subnet>10.102.100`) |
| Not started | Grafana/Prometheus/Loki (`--no-metrics`) |

Subnets are clear of stack 1 (172.29/16, 192.168.240.0/20, 10.213.100.x), stack 2
(172.31/16, 192.168.96.0/20), docker0, virbr0 and the host LAN.

## 2. Runbook

    S=/var/tmp/ionic-devmem/test-rocm102/scripts
    $S/stack.sh status              # containers, ports, guest kernel/ionic/amdgpu state
    $S/stack.sh cycle               # cold cycle: quick-start down, quick-start up, wait ssh, guest-prep
    $S/stack.sh down | up           # the halves (up refuses while guests run, see F4)
    $S/gssh 1 'uname -r'            # or: $S/gssh 2 - < script.sh ;  $S/gscp 1 FILE... DEST
    $S/guest-prep.sh                # after every boot (stack.sh up runs it), see F2
    $S/qs-test.sh LABEL [opts]      # ONE `quick-start test`; console in B/logs/qs-LABEL.log,
                                    # artifacts + report + NIC stats in B/evidence/quickstart/LABEL/
    $S/qs-test.sh base
    $S/qs-test.sh sdxi --sdxi-test
    $S/qs-test.sh hipfile --hipfile-test                              # ~7 min
    $S/qs-test.sh rdma-hostalloc --rocm-rdma-test
    $S/qs-test.sh rdma-dmabuf --rocm-rdma-test --rocm-rdma-mode dmabuf
    $S/qs-test.sh p4 --p4-test                                        # last, then stack.sh cycle
    $S/tcp-test.sh [MIB]            # ping + sha256-verified TCP both ways (python, no socat in the image)
    $S/devmem-baseline.sh LABEL     # ncdevmem + devmem_tx bind attempt and controls
    $S/guest-facts.sh LABEL         # everything in section 4, into B/evidence/facts/
    $S/stack.sh qt ARGS             # qemu-tool v2.1.0 with this stack's environment

`scripts/env.sh` holds the paths and the exact quick-start arguments:

    qt quick-start --xml Q/qemu/stacks/rocm-golden.xml --xml B/stack/r102-fleet.xml \
       --stack-dir B/stack/ionic-devmem-r102 --key-dir B/key --repo-root Q --no-metrics --yes VERB

**quick-start is the harness for everything**: preflight, up, test (all options), down. No
`gen-compose`/`compose`/`ansible-playbook` by hand was needed. What had to be added around it,
all outside qemu-tool:

1. `B/shim/docker` first in `PATH` (F1).
2. `BUILDX_BUILDER=default` (F3).
3. `docker-compose.override.yml` in the stack dir: `cgroup_parent: system.slice` on the ten
   services, and the `l2` subnet (quick-start cannot express either).
4. `guest-prep.sh` after each boot (F2).

A boot leaves the guests as the image ships them: amdgpu not loaded
(`amdgpu-probe --status` = `ready`; `nofirmware` on the very first boot), no fleet address.
Any `qs-test.sh` run configures them (firmware, `amdgpu-probe`, address) before testing.

## 3. Findings on the way up

- **F1. The docker CLI is the broken part, not dockerd.** `docker exec` and attached
  `docker run` print nothing (`docker -D exec` logs `[hijack] End of stdout` at once), but the
  same exec through the Engine API on `/var/run/docker.sock` returns the stream
  (`B/scripts/dexec-probe.py`). `B/shim/docker` does `exec` and attached `run` through the API
  (and adds the cgroup parent to `run`), and passes everything else to `/usr/bin/docker`. With
  it, the two things the study said could not work here do work unmodified: the playbook's
  firmware generation (`docker run ... | tar -xf -`, binary-safe, checked with a 3 MB hash) and
  quick-start's NIC byte-counter check; `quick-start down` (guest agent through `docker exec`)
  works too. Stack 1/2's "create/start/wait/logs" workaround is no longer needed for this stack.
- **F2. `quick-start test` fails its RDMA step on any `<ernic-subnet>` other than the default
  (real, upstream).** The image's boot unit always adds 192.168.100.(10+N); quick-start adds
  the fleet address beside it. The 192.168.100 address is GID index 1, perftest picks it, and
  the NIC server (given only the fleet's GIDs in `ERNIC_TCP_GUEST_GIDS`) correctly refuses:
  `ERROR: ionic_adminq MODIFY_QP 2: no mesh node owns destination gid ::ffff:c0a8:640c`,
  client `Failed to modify QP 2 to RTR`. First run failed exactly so
  (`B/evidence/quickstart/base-fail-gid-192.168.100/`). `guest-prep.sh` deletes the
  192.168.100 address at run time (not persistent); every test then passed.
- **Why not just use the default subnet (host quirk, re-confirmed on this stack).** On a
  fresh boot `ping 192.168.100.12` from r102-1: ARP resolves, 100 % loss, and tcpdump in
  r102-2 shows the requests arriving from **172.28.102.1** (this stack's bridge): stack 2's
  `l2` network 192.168.96.0/20 masquerades that range on every bridge
  (`B/evidence/stack/ping-192.168.100-default-subnet.txt`).
- **F3. compose could not build `tapsetup`** (`pull_policy: build`): the host's selected
  buildx builder `batesste-bake` (docker-container driver) cannot start its buildkit container
  (no cgroup parent). First `up` failed there, and quick-start "retried" by moving the SSH base
  to 2283 for good (`overlay-ports.xml`). `BUILDX_BUILDER=default` fixes the build;
  `stack.sh up` deletes `overlay-ports.xml` so the base returns to 2281.
- **F4. `quick-start up` on a running stack destroys it (real, upstream).** It finds its own
  two SSH ports "in use", moves the base (2283 -> 2285), re-renders, and compose recreates both
  qemu containers under the running guests; the new QEMUs exit 1 with `vfio-user:
  /run/vfu/rocjitsu-1.sock: timed out waiting for reply` because the servers were not
  restarted. Hit once by my own script (`B/evidence/stack/quick-start-up-on-running-stack.log`,
  `qemu-1-after-recreate.log`); the guests had been killed hard, booted cleanly afterwards.
  `stack.sh up` now refuses while a qemu container runs.
- **Open question closed: BAR0 fits at the default.** No `<vm-cpu-phys-bits>` /
  `<vm-pci-hole64-size>`: guest CPU reports 40 physical bits, GPU `02:00.0 [1002:75c1]` BAR0 =
  `0x4000000000` 256G, BAR2 2M, both assigned. QEMU line: `B/evidence/stack/qemu-1-cmdline.txt`
  (`-cpu EPYC`, `q35`, NVMe `vram-dev=rocjitsu-1,vram-bar=0`).

## 4. What the guest and server are (`B/evidence/facts/{00-fresh-boot,10-after-quickstart,20-final}-*`)

| Item | Seen |
| --- | --- |
| `uname -r` | `7.2.6-070206-generic` (#202609141300), cmdline has `modprobe.blacklist=amdgpu` |
| Stamp `/etc/rocm-golden-guest.json` | ROCm 10.2 (`amdrocm-runtime-dev10.2 10.2.0-2`, `/opt/rocm/core-10.2`), `ernic_source_pin 90434a2`, rdma-core 66.0 in `/usr/local`, perftest `e68c74e9`, fio-3.43 + libhipfile, `ip_discovery_bin: false` |
| amdgpu | `amdgpu-dkms 1:7.2.3.31700000-2428518.26.04`, module version `7.2.3.31700000`, srcversion `DCA345C99B7A6B4094FC2A0` |
| `/etc/amdgpu-emu-patches.json` | carried `kfd_atomics`, `ras_vbios`, `panel_type`, `amd_vsdb_freesync`; not needed `kfd_ptrace_gate` |
| `dkms status` | `amdgpu/7.2.3-2428518.26.04`, `ernic-p4/0.2.0`, `ionic-ernic/7.2.6` |
| lsmod at boot | `ionic` (used by `ernic_p4`, `ionic_rdma`), `ionic_rdma`, `ernic_p4`, `sdxi_char`, `nvme`; **no amdgpu** until `amdgpu-probe` (then amdgpu + amdttm/amdkcl/amdxcp...; `/dev/kfd`, `/dev/dri/renderD*`) |
| ionic srcversions | ionic `44164456D34C01FB438D570`, ionic_rdma `24E57766CB83B79E273C7B0`, ernic_p4 `66D2E2195859E80AE848D07` (= `/etc/ionic-srq-p4.json`), from `updates/dkms`, taint `OE` |
| `/usr/src` | `ionic-ernic-7.2.6/{Kbuild,dkms.conf,Module.symvers,eth,rdma,uapi}` plus built objects, `ernic-p4-0.2.0`, `ionic-ernic-p4` (kernel checkout), `amdgpu-7.2.3-...`, headers. Same layout as the older image: an in-guest rebuild is possible |
| `ethtool -i rocm-ernic0` | driver ionic, version 7.2.6-070206-generic, firmware `rocm-ernic-1.0`, bus 0000:01:00.0 |
| `ethtool -k` | **every Tx offload off and `[fixed]`**: tx-checksumming, scatter-gather, TSO; GSO `off [requested on]`; GRO on; rx-checksumming off |
| `ethtool -l / -g` | 4 combined; rings 1024/1024 (max rx 16384, tx 8192) |
| `ibv_devinfo -v` | `rocm-rdma-ernic0`, PORT_ACTIVE, Ethernet, `max_srq 256`, `max_srq_wr 65535`, `max_srq_sge 2`, max_qp 256, max_mr 2049, max_sge 8, active_mtu 1024 |
| Kernel config | `NET_DEVMEM=y UDMABUF=y PCI_P2PDMA=y ZONE_DEVICE=y DEVICE_PRIVATE=y DMA_SHARED_BUFFER=y DMABUF_HEAPS=y PAGE_POOL=y DEBUG_INFO_BTF=y DEBUG_INFO_BTF_MODULES=y KPROBES=y KPROBE_EVENTS=y FPROBE=y FTRACE=y FUNCTION_TRACER=y DYNAMIC_FTRACE=y BPF_SYSCALL=y HSA_AMD_P2P=y MODVERSIONS=y VLAN_8021Q=m`; **not set**: `DMA_API_DEBUG`, `DEBUG_NET`, `KASAN`, `PROVE_LOCKING`, `MODULE_SIG_FORCE`, `IKCONFIG` |
| BTF / tools | `/sys/kernel/btf/vmlinux` yes; **no `/sys/kernel/btf/ionic` or `amdgpu`** (DKMS modules carry no BTF: kprobes only). bpftrace, strace, gcc 15.2.0, make, iperf3, python3, nc present; **no socat, no pahole** |
| Devices | `/dev/udmabuf`, `/dev/ernic-p4`, `/dev/sdxi0`, `/dev/nvme0n1`, `/dev/infiniband/uverbs0` |
| Server | image binary `/usr/local/bin/rocm-ernic` sha256 `9e449469...`, no bind mount; `rocm-ernic -s ... -b tcp:manager:listen:6320 -m 72:6f:63:6d:00:01 -S ... --p4 --p4-guest-scope load -T tap0`; stats `nic_profile : compat` |
| **Server `/proc/1/maps`** | **`262144 MiB rw-s /memfd:rocjitsu-vram`** mapped beside guest RAM (`6144 MiB` + `2047 MiB /memfd:memory-backend-memfd`), on both servers, from first boot: the fork QEMU hands the NIC server the GPU BAR |

## 5. Upstream tests, as shipped (`B/evidence/quickstart/<label>/`)

All run as `qemu-tool quick-start ... test` through `qs-test.sh`, in this order, on one boot.

| Test | Command option | Result | Numbers |
| --- | --- | --- | --- |
| base, first try | none | **FAIL** (F2, cause found) | `Failed to modify QP 2 to RTR` |
| base (configure + GPU + RDMA + NVMe) | none | PASS | `vector-add: PASS` (gfx1250, 32 CUs, 0 mismatches) both guests; `ib_send_bw` 65536 x 1000: 1950.15 MiB/s; ernic 1 moved 65536000 bytes; NVMe write 20.0 / read 2049.0 and 2013.0 MiB/s |
| SDXI | `--sdxi-test` | PASS | 4194304 bytes through `/dev/sdxi0`, identical, both guests |
| hipFile | `--hipfile-test` | PASS | bench 205.7 and 210.2 MB/s, 33554432 bytes verified, `gemm-hipfile-bench: PASS`; fio libhipfile 212.4 and 214.4 MB/s (the fast, BAR-path rate; ~7 min wall) |
| ROCm RDMA host-alloc | `--rocm-rdma-test` | PASS | `ib_write_bw` 1158.35 MiB/s, `VALIDATION: PASSED - 7 chunks, 58720256 bytes` |
| ROCm RDMA dma-buf | `--rocm-rdma-test --rocm-rdma-mode dmabuf` | PASS | 1305.08 MiB/s, `VALIDATION: PASSED - 7 chunks, 58720256 bytes`, `ibv_reg_mr_ex with dmabuf` on both sides, `Use ROCm memory : ON` |
| ROCm RDMA vram | `--rocm-rdma-test --rocm-rdma-mode vram` | expected FAIL (rc 1) | `Couldn't allocate MR with error=14`; quick-start reports "known limitation" |
| P4 | `--p4-test` | PASS | fp8 reduction 170.5 MB/s, 262144 elements x 2000, avg 1.537 ms |
| base after cold cycle | none | PASS | `ib_send_bw` 1249.71 MiB/s; NVMe read 1560 / 1729 MiB/s |

Every optional run repeats the base tests first. `ib_send_bw` average across the eight runs:
1134 to 1950 MiB/s (1950, 1539, 1546, 1671, 1815, 1134, 1424, 1250): treat anything in that
band as normal, not as a regression. NVMe write is 20-21 MiB/s every time; read 1528-2049.

**NOT RUN**: `ib_read_bw` variant (`-e quickstart_rocm_rdma_tool=ib_read_bw`, not a
quick-start option), live GPU validation (`quickstart_rocm_rdma_validation=live`),
`--rdma-duration` / `--nvme-duration` soaks, flavors `mi355x` / `helios`, the report/Grafana
side (`--metrics`), `qemu-tool check`, upstream's `scripts/check-vm-devices.sh` and the
`generate-vm-report.sh` benches.

## 6. devmem baseline on the unmodified guest (`B/evidence/devmem-baseline/`)

Tools copied in unchanged from stack 1 (`B/bin/devmem_tx` sha256 `1579ebae...`, `ncdevmem`
`1cdf3e86...`) to `~/devmem` in both guests (they stay on the guest disks).

- **Bind fails as expected**, r102-1 -> r102-2, shipped ionic (`44164456...`):
  - `ncdevmem -f rocm-ernic0 -s 10.102.100.12 -p 5310 -t 0 -q 1`: `netdev_bind_tx: Operation
    not supported`, `YNL failed: Kernel error: 'Driver does not support netmem TX'`, exit 1; the
    receiver got nothing.
  - `devmem_tx -m devmem`: `bind-tx: netlink error -95 ... extack='Driver does not support
    netmem TX'`, `RESULT tx BIND-FAILED`; strace shows `error=-EOPNOTSUPP` with that
    `NLMSGERR_ATTR_MSG`.
- **Controls with the same framing pass**, verified byte for byte by the receiver:
  `-m copy` and `-m zc` (MSG_ZEROCOPY), 256 x 64 KiB = 16777216 bytes, `RESULT rx PASS ...
  mismatches=0`. With `-m zc`: `completed=256 notifications=32`, `copied=8` and `copied=16` in the two runs.
- **Plain TCP, sha256 at the receiver**: 64 MiB each way, hashes equal (`tcp-test-stock-ionic.txt`,
  and again after the cold cycle, `tcp-test-final.txt`); ping 0 % loss.
- Kernel logs: no BUG/WARNING/Oops/Call Trace in either guest after all of the above.

## 7. Blocked / not done for the devmem work on this stack

1. **devmem TX needs two swaps, neither made** (stack is pristine):
   - the server: the image's `ernic.90434a2` grants no Tx offload (table above), so a
     feat/devmem-poc build must be bind-mounted from the override (study 7.3). **NOT RUN**;
     `build-ernic.sh` was not copied/adapted to `B/scripts`.
   - the guest ionic: `ionic-ernic/7.2.6` at `90434a2` has no devmem patch and lacks srq
     0010/0011. In-guest rebuild from `/usr/src/ionic-ernic-7.2.6` + `devmem-tx` looks feasible
     (sources, headers, gcc present). **NOT RUN**; `ionic-mod.sh` not adapted. `ernic_p4` holds
     `ionic` and must be unloaded first; whether the baked `ernic_p4.ko` reloads is unknown.
2. On 7.2.6 the net core attaches a dma-buf statically, so an amdgpu buffer is pinned to GTT
   and devmem TX would read system pages, not VRAM (`notes/test-gpu-dmabuf.md`). The RDMA
   dma-buf path passing here does not change that. Untested with amdgpu 7.2.3.
3. No per-descriptor proof that RDMA dma-buf traffic touched the GPU BAR: upstream's evidence
   is payload validation plus the `rocjitsu-vram` mapping; no address probe was run.
4. Debug coverage is thinner than stacks 1/2: no `DMA_API_DEBUG`, `DEBUG_NET`, KASAN or lockdep
   in this kernel, and no module BTF.
5. This stack only works while stack 2 holds 192.168.96.0/20 because of `guest-prep.sh`; if
   upstream's default subnet is wanted, stack 2's `l2` network must move first.
6. Worth reporting upstream (nothing was posted): F2, F4, and that F3's failed build turns
   into a permanent port move.

## 8. Files

    B/scripts/  env.sh stack.sh gssh gscp guest-prep.sh qs-test.sh tcp-test.sh tcp_hash.py
                devmem-baseline.sh guest-facts.sh dexec-probe.py
    B/shim/docker                      docker CLI shim (F1)
    B/stack/r102-fleet.xml             overlay fleet
    B/stack/ionic-devmem-r102/         rendered stack, docker-compose.override.yml, artifacts/
    B/images/ B/key/                   backing + overlays + NVMe images; guest keypair
    B/bin/ B/src/                      devmem_tx, ncdevmem; sources incl. gpu/devmem_tx_gpu.c (copies)
    B/logs/                            preflight, up-*, down-*, qs-*.log
    B/evidence/facts/                  guest + server facts at three points
    B/evidence/quickstart/<label>/     console.log, r102-N.md, rocm-rdma.md, report/, NIC stats before/after, run.txt
    B/evidence/devmem-baseline/        bind failure, controls, hashed TCP
    B/evidence/stack/                  rendered compose, overlays, QEMU command line, F2/F4 evidence
    B/evidence/study/                  the earlier read-only study's evidence
