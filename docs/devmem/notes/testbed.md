# ionic devmem-TX test bed: runbook

**Read sections 12 and 11 first. Section 12 (2026-11-19): state after the devmem TX matrix on
7.4-rc3; the stack is now on `7.4.0-rc3-devmem-kasan`. Section 11: a host problem that still
breaks 192.168.100.x between the guests of this stack.**

State as left on 2026-11-18 (after the emulator TX-offload validation, section 10):
**stack RUNNING**, both guests on `7.2.15-devmem` with the kernel's own (unpatched)
in-tree ionic, servers = the locally built
`/var/tmp/ionic-devmem/test/ernic/tx-offloads-1/build/rocm-ernic` (bind-mounted;
`stack.sh server image` returns to the image's ernic.90434a2). With that server the
netdev has SG, HW checksum and TSO; statements below that every offload is off
describe the image's server. Everything is under `/var/tmp/ionic-devmem/test/`
(`T` below). Nothing on the host outside docker was created: no sudo was used, no
host bridge or tap exists.

## 1. What it is

qemu-tool **2.0.0**, run with uvx from the scratch clone (never the stale
`~/.local/bin/qemu-tool` 1.4.1):

    uvx --from /var/tmp/ionic-devmem/qemu-minimal/qemu qemu-tool <args>
    T/scripts/stack.sh qt <args>          # same thing

2.0.0 has no `vfio-user-ernic-2vm` stack any more (the hand-written stacks
are deleted). A fleet is `qemu/stacks/rocm-golden.xml` plus a local overlay,
rendered with `gen-compose --output-dir` and driven with
`compose --stack-dir`. This one:

| Item | Value |
| --- | --- |
| Overlay XML | `T/stack/devmem-fleet.xml` |
| Rendered stack / compose project | `T/stack/ionic-devmem/` / `ionic-devmem` |
| Local compose override | `T/stack/ionic-devmem/docker-compose.override.yml` (written by `stack.sh`) |
| Guests | `devmem-1`, `devmem-2` (hostnames `ernic-vm-1/2`), 8 vCPU, 8 GiB |
| Disks | `T/images/devmem-{1,2}.qcow2`, overlays on `T/images/devmem-1-backing.qcow2` (pulled `qemu-tool-rocm-golden:20261117.g481c892`, Ubuntu 26.04) |
| SSH | `ssh -i T/images/keys/id_rsa -p 2261 ubuntu@127.0.0.1` (devmem-1), port 2262 (devmem-2); `T/scripts/gssh 1 [cmd]`; passwordless sudo |
| ionic netdev | `rocm-ernic0`, 192.168.100.11 (devmem-1), 192.168.100.12 (devmem-2), set at boot by the image's `rocm-ernic-netcfg.service` |
| NIC MACs | 72:6f:63:6d:00:01 / :02 |
| Host share | `/var/tmp/ionic-devmem` -> guest `/mnt/hostfs` (9p, tag `hostfs`, **read-only**, in fstab) |
| Containers | `ionic-devmem-{ernic,rocjitsu,qemu,tapsetup}-{1,2}-1` |
| Docker networks | `ionic-devmem_fleet` 172.29.0.0/16 (mesh, fixed addresses), `ionic-devmem_l2` (guest Ethernet) |
| Mesh port | 6331, inside the compose network only (not published; host 6320/5000 untouched) |
| Published host ports | 127.0.0.1:2261, 127.0.0.1:2262 only (no Prometheus/Grafana/Loki) |

Ethernet path (how the two ionic netdevs reach each other): each `ernic-N`
container runs `rocm-ernic ... -T tap0`; its `tapsetup-N` sidecar (NET_ADMIN,
exits at once) creates `tap0` in that container's netns and bridges it to the
container's interface on the shared docker network `l2`. Guest frames go
tap0 -> br0 -> l2 -> br0 -> tap0. The TCP mesh (`-b tcp:manager/worker`,
network `fleet`) carries RDMA only. No host tap/bridge, so no sudo.

Each guest also has one rocjitsu GPU: `gen-compose` refuses
`<rocjitsu-per-vm>0`. It is unused (`modprobe.blacklist=amdgpu` is the
image's default).

## 2. Day-to-day commands

    S=/var/tmp/ionic-devmem/test/scripts
    $S/stack.sh status        # qemu-tool list, compose ps, which server binary
    $S/stack.sh down          # guest-agent poweroff, then compose down (disks kept)
    $S/stack.sh up            # compose up -d, waits for ssh on both guests
    $S/stack.sh restart
    $S/stack.sh render        # re-render after editing devmem-fleet.xml
    $S/stack.sh compose logs --tail 50 ernic-1
    $S/gssh 1 'uname -r'      # or: $S/gssh 2 - < script.sh
    $S/tcp-test.sh [MB]       # ping + socat/sha256 transfer, both directions
    $S/baseline.sh LABEL      # ethtool -i/-k/-l + ncdevmem TX attempt 1 -> 2

Raw equivalents of up/down:

    QT="uvx --from /var/tmp/ionic-devmem/qemu-minimal/qemu qemu-tool"
    X="--xml /var/tmp/ionic-devmem/qemu-minimal/qemu/stacks/rocm-golden.xml --xml /var/tmp/ionic-devmem/test/stack/devmem-fleet.xml"
    $QT compose --stack-dir /var/tmp/ionic-devmem/test/stack/ionic-devmem $X up -d
    $QT down ionic-devmem --yes      # see quirk 2 below: reports failure here
    $QT compose --stack-dir /var/tmp/ionic-devmem/test/stack/ionic-devmem $X down

`--xml` is needed on commands that start the stack: the images directory and
the qemu-tool source mount (`<qemu-tool-src>`, so the qemu containers install
run-vm 2.0.0 from the scratch clone, not 1.x from PyPI) are not baked into
the rendered YAML.

Rebuild from nothing (what was run):

    $QT gen-compose $X --output-dir /var/tmp/ionic-devmem/test/stack/ionic-devmem
    $QT gen-vm $X --vm-name devmem-1                      # pulls the backing image (30 s)
    $QT gen-vm $X --vm-name devmem-2 --backing-file /var/tmp/ionic-devmem/test/images/devmem-1-backing.qcow2
    $S/stack.sh up
    # per guest, once: apt-get update && apt-get install socat; fstab line for hostfs
    $S/install-kernel.sh 7.2.15

Fresh guest disks without re-pulling: `stack.sh down`, then
`$QT gen-vm $X --vm-name devmem-N --restore-image` (discards kernels, socat,
fstab line, ~/ionic-mod; redo the last two lines above).

Full removal: `stack.sh down`; the images are plain files in `T/images`
(8.4 GB backing). Images `qemu-tool/ernic-tapsetup:1`, `devmem-kbuild:1` and
the three pulled `20261112.gdca6903` images stay in docker.

### Host quirks found (all worked around in the scripts)

1. **dockerd refuses any container with no cgroup parent** ("cgroup-parent
   for systemd cgroup should be a valid slice named as xxx.slice"), `docker
   run` and compose alike. `qemu-tool check` does not test this and passes.
   Fix: the override file gives every service `cgroup_parent: system.slice`;
   `scripts/drun.sh` does the same for one-off containers. Cause not
   investigated.
2. **`docker exec` / attached `docker run` deliver no stdout** in this
   session (`docker logs` works). Consequence: `qemu-tool down <stack>` cannot
   read the guest agent's reply, prints "shutdown not requested ()" and
   "no guest accepted a shutdown request", exits 1 and does not remove the
   stack, although the guests do power off (guest journal: `qemu-ga:
   guest-shutdown called`). `stack.sh down` ignores that status, waits for the
   qemu containers to exit, then runs compose down. Not checked whether an
   interactive terminal behaves the same.
3. A guest reboot can hang an ssh session forever (SLIRP sends no reset);
   `gssh` sets ServerAlive, and `install-kernel.sh` reboots detached.

## 3. Kernels

Built on this host, out-of-tree, in an Ubuntu 26.04 container (gcc 15.2.0,
the guests' compiler; the host has no bison): `scripts/kbuild.sh <flavour>`
runs `scripts/kbuild-in-container.sh` in image `devmem-kbuild:1`
(`T/builder/Dockerfile`). Sources are scratch copies, never the shared trees:

| Flavour | Source | Release | Debs |
| --- | --- | --- | --- |
| 7.2.15 | `T/kbuild/7.2.15/src` = v7.2.15 + the bundle's 20 `dmabuf-72x-backport` patches (`git apply`, 0019 with `-C1`; `scripts/apply-bundle-patches.sh`) | `7.2.15-devmem` | `T/kbuild/7.2.15/linux-{image,headers}-7.2.15-devmem_7.2.15-2_amd64.deb` |
| 7.4-rc3 | `T/kbuild/7.4-rc3/src` = v7.4-rc3, unmodified | `7.4.0-rc3-devmem-kasan` | `T/kbuild/7.4-rc3/linux-{image,headers}-7.4.0-rc3-devmem-kasan_7.4.0~rc3-1_amd64.deb` |

Checked: `git diff --stat <tag>` against each copy shows only the backport
(29 files, 5 new) for 7.2.15 and nothing for 7.4-rc3. The earlier interrupted
run made the copies and the first build; this run found the two build
containers finishing a second (incremental) pass and verified the result
rather than rebuilding from scratch.

Config = `make defconfig` + kernel-tools `config-ernic-bundle` (pulls in
config-default, config-amdgpu-dkms, config-infiniband) + additions, then
`olddefconfig`. In both `.config`s: UDMABUF=y NET_DEVMEM=y PAGE_POOL=y
MEMFD_CREATE=y DEBUG_NET=y DMA_API_DEBUG=y FTRACE/FUNCTION_TRACER=y KPROBES=y
KPROBE_EVENTS=y FPROBE=y BPF_SYSCALL=y BPF_JIT=y DEBUG_INFO_BTF=y
DEBUG_INFO_BTF_MODULES=y IONIC=m INFINIBAND=m 9P/virtio built in.
7.4-rc3 adds KASAN=y (generic, inline) PROVE_LOCKING=y DEBUG_ATOMIC_SLEEP=y,
KFENCE off. Not set in either: DMA_API_DEBUG_SG (olddefconfig dropped it),
INFINIBAND_IONIC (so an in-tree boot has no ionic_rdma / RDMA device).

Boot mechanism: **debs installed in the guest + GRUB** (qemu-tool 2.0.0
run-vm has no x86 `-kernel`). The image pins its kernel with
`GRUB_TOP_LEVEL` in `/etc/default/grub.d/99-rocm-ernic-kernel.cfg`;
`install-kernel.sh` writes `zz-devmem.cfg` after it.

    $S/install-kernel.sh 7.2.15     # both guests; installs if missing, selects, reboots, waits
    $S/install-kernel.sh 7.4-rc3
    $S/install-kernel.sh stock      # back to the image's 7.2.6-070206-generic

All three kernels are installed in both guests; switching takes ~30 s. A
first install takes ~3.5 min because the image's amdgpu DKMS module
autoinstalls and fails to build against these kernels
(`/etc/dkms/no-autoinstall-errors` is set so dpkg still finishes; amdgpu is
blacklisted and unused).

ionic per kernel: stock 7.2.6 = the image's DKMS `ionic-ernic/7.2.6`
(v7.2.6 + srq + p4, with ionic_rdma). `7.2.15-devmem` and
`7.4.0-rc3-devmem-kasan` = the kernel's own in-tree ionic. The SRQ/p4 DKMS
patches target 7.2.x, so on 7.4-rc3 only the in-tree ionic is used. The
emulator (both ernic.90434a2 and develop bd902345) works with all three for
Ethernet.

## 4. Guest tools

Both guests: socat (apt, added by this run), iperf3, ethtool, bpftrace,
strace, gcc/make/git/dkms (image), `/usr/local/bin/ncdevmem`.

ncdevmem: static, from the v7.4-rc3 selftests, built by
`scripts/build-ncdevmem.sh` from a `git archive` subset in `T/ncdevmem/src`
(no checkout touched) -> `T/bin/ncdevmem`
(sha256 1cdf3e86...10ef4db); `baseline.sh` installs it. On 7.2.15-devmem:
`bpftrace -e 'BEGIN{...}'` runs, `kprobe:ionic_start_xmit` is listed,
`/sys/kernel/btf/{vmlinux,ionic}` exist. A module built in the guest has no
BTF (no pahole/vmlinux there), so use kprobes, not fentry, on a patched
module.

## 5. Baseline evidence (unpatched driver), in `T/evidence/`

`baseline-7.2.15-devmem-intree.txt`, `baseline-final-...` (same state after
the last restart), `baseline-7.2.15-devmem-ernic-dkms-ionic.txt` (v7.2.6 +
srq + p4), `baseline-7.4.0-rc3-devmem-kasan-intree.txt`. Identical outcome
in all:

    $ sudo ncdevmem -f rocm-ernic0 -s 192.168.100.12 -p 5302 -t 0 -q 1 < /tmp/devmem-tx.txt
    using ifindex=3
    netdev_bind_tx: Operation not supported
    YNL failed: Kernel error: 'Driver does not support netmem TX'
    ncdevmem: Failed to bind: Operation not supported
    exit status: 1

strace of the same run shows the netlink reply `error=-EOPNOTSUPP` with
`NLMSGERR_ATTR_MSG "Driver does not support netmem TX"`. Nothing reaches the
receiver. `ethtool -i`: driver ionic, version = kernel release,
firmware-version rocm-ernic-1.0, bus 0000:01:00.0. `ethtool -k`: every
offload off, e.g. `tx-checksumming: off`, `scatter-gather: off`,
`tx-scatter-gather: off [fixed]`, `tcp-segmentation-offload: off`;
only `generic-segmentation-offload: off [requested on]` is not fixed. So
note B1 holds in the guest: the emulator advertises no SG/CSUM.
`ethtool -l`: 4 combined channels.

`tcp-test-*.txt`: ping 0% loss and a 64 MiB socat transfer with matching
sha256 in both directions on stock 7.2.6, 7.2.15-devmem (in-tree ionic and
the ernic DKMS-source ionic), 7.4.0-rc3-devmem-kasan, and with the locally
built server. `splat-check-7.4.0-rc3-devmem-kasan.txt`: 0 KASAN/BUG/WARNING/
lockdep lines in that boot's kernel log after the TCP tests and a module
swap.

## 6. Swapping in a locally built rocm-ernic server

How the stack picks the server: the `ernic-N` services run the image named
by `<ernic-ref>` and exec `rocm-ernic` from its PATH
(`/usr/local/bin/rocm-ernic`). There is no setting for a binary path, and
`gen-compose` refuses an `<ernic-ref>` whose build stamp or `vfu.<sha>`
differs from `<qemu-ref>`'s, so a private image tag is awkward. The binary is
therefore bind-mounted over the image's, through the compose override.
It **must be built against the image's libvfio-user fork** (vfu.3719c3a,
Ubuntu 24.04), i.e. inside that image; a host build links the host's
`/usr/local` libvfio-user.

    # 1. build from a snapshot; never builds in or writes to the checkout
    $S/build-ernic.sh poc1                 # working tree as it is now (uncommitted + untracked files)
    $S/build-ernic.sh dev bd902345         # or: a committed ref (git archive)
    #    -> T/ernic/<label>/build/rocm-ernic, log T/ernic/<label>/build.log, T/ernic/<label>/SOURCE
    # 2. select it; restarts the whole stack (clean guest poweroff, ~35 s)
    $S/stack.sh server /var/tmp/ionic-devmem/test/ernic/poc1/build/rocm-ernic
    $S/stack.sh status                     # shows the bind mount
    # 3. back to the image's binary
    $S/stack.sh server image

`ERNIC_REPO=<dir>` overrides the checkout (default
`~/Projects/rocm-ernic`). The build is `cmake -G Ninja
-DCMAKE_BUILD_TYPE=Debug -DERNIC_BUILD_P4=ON`, target `rocm-ernic` only (no
tests, no -Werror). Always restart the whole stack: restarting `ernic-1`
(the mesh manager) alone wedges the guests (qemu-minimal known issue 7).
Server logs: `stack.sh compose logs ernic-1`; the default log level is warn,
and extra server options would need the rendered entrypoint changed.

Tested with a build of committed `bd902345` (develop tip, the branch's HEAD
commit): stack came up, TCP test passed, then switched back. The
working-tree mode was run once as well (`T/ernic/poc-snapshot-1`, the
feat/devmem-poc tree as it stood at about 19:15 EST, mid-edit): it compiled
and linked, and `git status` of the checkout was identical before and after.
**That binary was not deployed or run**; whether the in-progress emulator
works in the stack is untested.

## 7. Building and loading a patched ionic module

    $S/ionic-mod.sh all  NAME BASE SERIES_DIR...   # stage + build + load, both guests
    $S/ionic-mod.sh stage|build|load NAME ...      # the steps separately
    $S/ionic-mod.sh restore                        # back to the installed module

`BASE`:

- `intree-7.4-rc3` / `intree-7.2.15`: the Ethernet driver from that test
  kernel's source; builds `ionic.ko`. Use `intree-7.4-rc3` for a mainline
  series (`/var/tmp/ionic-devmem/patches/mainline`) with the guests on
  `7.4.0-rc3-devmem-kasan`.
- `ernic-7.2.6`: the rocm-ernic DKMS source set: v7.2.6 ionic eth + rdma +
  uapi from `/var/tmp/ionic-devmem/linux-7.2.6`, plus `patches/srq` then
  `patches/p4` from the pin, then the series, staged with the Kbuild
  `setup-ionic-dkms.sh` writes; builds `ionic.ko` + `ionic_rdma.ko`. For the
  guests on `7.2.15-devmem` (or stock 7.2.6). With no series the staged tree
  is byte-identical to the image's `/usr/src/ionic-ernic-7.2.6` (checked
  with `diff -r`).

Patches are kernel-root relative (`-p1`), applied on the host with
`git apply` in a plain directory (no repository, no commit); files without a
diff (cover letter) are skipped; a patch touching anything outside the
staged ionic directories fails. The build is `make -C
/lib/modules/$(uname -r)/build M=~/ionic-mod/NAME modules` in each guest
; load is `rmmod ionic_rdma; rmmod ionic; insmod`, then the image's
netcfg service re-adds the address. Not persistent across a reboot. A core
(non-ionic) change needs a kernel rebuild instead: edit
`T/kbuild/<flavour>/src`, `scripts/kbuild.sh <flavour>` (incremental,
~4 min), then reinstall the debs by hand (`install-kernel.sh` skips an
already-installed release; the deb revision increments).

Tested: `ernic-7.2.6` with no series on 7.2.15-devmem (loads, RDMA link
`rocm-rdma-ernic0` ACTIVE, TCP test passes); a one-line marker patch
(`T/examples/marker-series`) on `intree-7.2.15` and `intree-7.4-rc3`
(marker line in dmesg on both guests, TCP test passes, restore works).
**Not tested:** the real devmem series in `patches/mainline` (another
engineer's work in progress; not applied or built here).

The repository's own DKMS route (`fetch-ionic-sources.sh` + in-guest
`setup-ionic-dkms.sh`) was **not run**: `fetch-ionic-sources.sh` applies
with `git am`, which commits in its scratch clone, and it wants mbox
patches. If wanted: on the host
`fetch-ionic-sources.sh --source-dir D --kernel-ref v7.2.6 --patches-dir
<pin>/srq --patches-dir <pin>/p4 --patches-dir SERIES`, copy `D` and the
script into the guest, `setup-ionic-dkms.sh --source-dir D --kernel-ref
v7.2.6` there (replaces the image's `ionic-ernic/7.2.6` DKMS tree, builds
for the running kernel only).

## 8. What works / what is blocked

Works (run and seen):

- Two guests up through qemu-tool 2.0.0 with one rocm-ernic ionic NIC each;
  ping and hashed TCP transfers both ways over `rocm-ernic0`.
- Guest kernels 7.2.15-devmem and 7.4.0-rc3-devmem-kasan built, installed and
  booted in both guests; Ethernet works on both with the in-tree ionic.
- Static ncdevmem in both guests; baseline EOPNOTSUPP captured on three
  driver/kernel combinations.
- Server swap, patched-module build/load/restore, stack down/up, kernel
  switch: scripted and each exercised at least once.

Blocked / not possible / not done:

- **devmem TX data path**: needs (a) the ionic patch series (sets
  `netmem_tx`), and (b) the emulator advertising TX_SG + TX_CSUM and
  checksumming CSUM_PARTIAL frames; with today's emulator the netdev has no
  SG and no HW csum. Neither was built or tested here.
- NIC-only guests are not expressible in gen-compose 2.0.0 (rocjitsu >= 1).
- amdgpu DKMS does not build for the two test kernels (unused).
- `qemu-tool down` misreports on this host (quirk 2); use `stack.sh down`.
- `qemu-tool check` warns: Docker 24.0.7 publishes 127.0.0.1 ports in a way
  reachable from the local network segment; the guest key and password are
  public (published image), so ports 2261/2262 are root in the guests for
  anyone on the segment.
- Not run: iperf3, bpftrace probes on live traffic, any RDMA traffic test,
  `quick-start` (its test playbook targets the full rocm-golden fleet), the
  mesh under the locally built server beyond the stack's health checks.

## 9. Files

    T/scripts/   stack.sh gssh tcp-test.sh baseline.sh install-kernel.sh ionic-mod.sh
                 build-ernic.sh build-ncdevmem.sh kbuild.sh kbuild-in-container.sh
                 apply-bundle-patches.sh drun.sh
    T/stack/     devmem-fleet.xml, ionic-devmem/ (rendered), server-binary.path
    T/images/    backing + overlays + keys/
    T/kbuild/    <flavour>/{src,out,*.deb}, config-scripts/ (copy of kernel-tools')
    T/ernic/     <label>/{src,build,build.log,SOURCE}   (develop-bd902345, poc-snapshot-1 present)
    T/ionic-mod/ <name>/{tree,mod,SOURCE}                (ernic-base, marker-7.2.15, marker-7.4-rc3)
    T/bin/ncdevmem, T/ncdevmem/src, T/examples/marker-series
    T/evidence/, T/logs/
    T/scratch/   probes from this and the interrupted run (composeprobe, man page dump, render dry-run)

## 10. Added by the emulator TX-offload validation (2026-11-18)

Results: `notes/test-emulator-offloads.md`; evidence `T/evidence/emu-offloads/`.

- **Jumbo frames.** `stack.sh` now writes `networks: l2: driver_opts:
  com.docker.network.driver.mtu: "9216"` into the override and, on `up`, runs
  `stack.sh jumbo`: a one-off container (`qemu-tool/ernic-tapsetup:1`, NET_ADMIN
  only, `--network container:ionic-devmem-ernic-N-1`) that sets `tap0` and `br0`
  to MTU 9216. Guest MTUs up to the ionic maximum (9198) then pass; before, frames
  above 1500 died on the docker bridge. Guests still boot at MTU 1500.
- **Kernel gaps.** `7.2.15-devmem` and `7.4.0-rc3-devmem-kasan` have no
  `CONFIG_VLAN_8021Q`, no `NET_SCH_*` AQM (fq_codel, netem) and no tc actions. VLAN
  and CE-marking tests ran on the stock `7.2.6-070206-generic` kernel
  (`install-kernel.sh stock`), which has them and autoloads the DKMS ionic.
- **Reading emulator stats** (docker exec has no stdout here):
  `docker cp ionic-devmem-ernic-1-1:/run/ernic-stats/ernic-1.stats - | tar -xO`;
  the file is rewritten every second.
- **Scripts**, `T/scripts/emu-offloads/`:
  - `run.py LABEL [CONFIG[:FAMILY]...]`: the hashed matrix (configs `default gso-off
    tso-off tso-gso-off sg-off tx-off ecn ecn-ce mtu9000 mtu9198 mtu9000-tso-off`;
    families `v4 v6 vlan4 vlan6`); ~35 s a suite, ~110 s for TSO-off suites under
    KASAN. Resets offloads, MTU and addresses at the start of each suite and leaves
    the last suite's settings: reset by hand afterwards
    (`ethtool -K rocm-ernic0 tx on; ... sg on; ... tso on gso on gro on`, MTU 1500).
    Adds `fd00:100::1N/64` to `rocm-ernic0`.
  - `gt.py` (guest side, read from `/mnt/hostfs`): `rx PORT LOG`, `tx DST PORT
    SIZE...` (`--mode plain|small|sendfile|zerocopy`, `--mss`, `--v6opts`), `udp`.
  - `extras.sh OUT` (iperf3, msg_zerocopy selftest, rping; `KSRC=` picks the kernel
    source for the selftest), `throughput.sh OUT`, `jitter-probe.sh OUT`,
    `rx-overrun-probe.sh OUT [RING...]`, `cwr-check.sh CAPTURE...`.
- **Known testbed behaviour.** Each VM stalls 4-11 ms every 5.12 s (seen on the
  SLIRP NIC too, and with the pre-change server): expect an occasional spurious
  TCP tail-loss probe. With the tx-offloads-1 server, bulk TCP can overrun the
  receiver's 1024-entry Rx ring (`hw_rx_dropped`, emulator `rx_queue_empty`,
  matching fast retransmits); `ethtool -G rocm-ernic0 rx 4096` avoids it.
- Do not wait on `pgrep -f PATTERN` from a shell whose own command line contains
  PATTERN (it matches itself), and do not `pkill -f` that way over `gssh`.

## 11. Added by the devmem TX matrix on 7.2.15-devmem (2026-11-18, late)

Results: `notes/test-tx-7.2.15.md`; evidence `T/evidence/tx-7.2.15/`.

- **State.** Stack up (restarted once), both guests on `7.2.15-devmem` with the **patched**
  DKMS-source ionic loaded: `ionic-mod.sh load ernic-dmtx1` (= v7.2.6 + srq + p4 +
  `patches/ernic-v7.2.6`; staged in `T/ionic-mod/ernic-dmtx1`, built in both guests). A reboot
  returns to the in-tree ionic. `ernic-dmtx1-nobackstop` (the series without patch 0006, for
  comparison only) is staged and built in guest 1.
- **192.168.100.x between the two guests is broken by the host since 22:52 EST.** Stack 2's l2
  docker network (`ionic-devmem-rx_l2`) was recreated on 192.168.96.0/20, which contains
  192.168.100.0/24; bridge netfilter is on, so Docker's MASQUERADE rule for that network rewrites
  the source of this stack's bridged guest frames to this stack's bridge address (192.168.128.1).
  Symptom: ARP resolves, ping and TCP get no answer; `tcpdump` in guest 2 shows source
  192.168.128.1. Check: `docker network inspect ionic-devmem-rx_l2 ionic-devmem_l2 --format
  '{{.Name}} {{range .IPAM.Config}}{{.Subnet}}{{end}}'`. Whichever stack's l2 network holds
  192.168.96.0/20 breaks the other stack. Cure: recreate the offending network on another subnet
  (pin one in the compose override of both stacks) or, as root, turn bridge netfilter off for the
  bridges. Workaround used here: `ip addr add 10.213.100.1N/24 dev rocm-ernic0` in guest N (not
  persistent; `ALT=1` makes the devmem-tx harness add and use them).
- **Scripts**, `T/scripts/devmem-tx/`: `lib.sh` (`devmem_case`: one case with receiver,
  bpftrace probe, PFN check, counter diff, DMA-API check, kernel log since a marker; `CAP=1`
  adds captures, `HOOK=`/`HOOK_DELAY=` run something mid-stream), `probe.bt`, `m3-m5.sh`,
  `m6.sh pos|pos2|neg|edge`, `m7.sh tx|sg|both|gso`, `m7x.sh nobackstop|orphan`, `m8.sh`,
  `m9.sh xact|linkdown|reconfig`, `m9-rmmod.sh a|b`, `m10.sh [SECS]`, `summarize.py FILE...`,
  `pcapsum.py`, `install.sh` (copies `T/bin/devmem_tx` into the guests; rerun after a rebuild).
- **Programs.** `T/src/devmem_tx.c` -> `T/bin/devmem_tx` (devmem TX sender with arbitrary
  offsets/iovecs and a verifying receiver; options at the top of the source);
  `T/src/pcapcap.c` -> `T/bin/pcapcap` (static capture tool). Host-side capture: a one-off
  container in an emulator's network namespace on `tap0` (`cap_start N PORT` / `cap_stop N FILE`
  in `lib.sh`; container `ionic-devmem-cap-N`, removed by `cap_stop`).
- **Things that bite.**
  - Do not run a binary straight from `/mnt/hostfs` after rebuilding it on the host: the guest
    can execute stale pages (it crashed once). Install it into the guest.
  - `rmmod ionic` while a process holds a devmem TX binding oopses the guest kernel when that
    process exits (core bug at v7.2.15 and v7.4-rc3). Restart the stack afterwards.
  - A stalled devmem connection (frag limit, offloads off) lingers as an orphan for 75-80 s
    after its process exits and keeps its dmabuf; when the offloads come back it retransmits
    into whatever test runs next. Wait for `/sys/kernel/debug/dma_buf/bufinfo` to show 0 objects.
  - With `DMA_API_DEBUG`, each bind of a udmabuf can log "mapping sg segment longer than device
    claims to support". `devmem_tx -D` sets `num_errors` to 0 around the bind and back to 100,
    so other DMA-API reports still print.
  - `ionic-mod.sh load` clears the kernel log; save `dmesg` first if it matters.

## 12. Added by the devmem TX matrix on 7.4.0-rc3 (2026-11-19)

Results: `notes/test-tx-7.4-rc3.md`; evidence `T/evidence/tx-7.4-rc3/`.

- **State.** Stack up (restarted once, after the module-unload oops), both guests on
  `7.4.0-rc3-devmem-kasan` (GRUB default now) with the patched in-tree ionic loaded:
  `ionic-mod.sh load dmtx74-1` (= in-tree ionic + `patches/mainline` 0001..0006; staged in
  `T/ionic-mod/dmtx74-1`, built in both guests). A reboot returns to the unpatched in-tree ionic.
  Also built in guest 1: `dmtx74-1-nobackstop` (series without 0005) and `dmtx74-1-xdpfix`
  (series + `patches/xdp-frag-offset/net`). 192.168.100.x is still broken (section 11); both guests
  have 10.213.100.1N/24 and the harness was run with `ALT=1`. `7.2.15`: `install-kernel.sh 7.2.15`,
  then `ionic-mod.sh load ernic-dmtx1`.
- **`ionic-mod.sh`** now stages `Documentation/.../pensando/ionic.rst` for `intree-*` bases, so a
  mainline series that patches the driver's documentation applies.
- **Harness** (`T/scripts/devmem-tx/`): set `E=T/evidence/<dir>` to choose the evidence directory
  (default is still tx-7.2.15). New: `m-xdp.sh basic|frags|toggle` (XDP_PASS / XDP_TX /
  XDP_REDIRECT on the sender, one queue, while a devmem stream runs), `m-xdp-diag.sh NAME OBJ SEC
  MODE` (captures on both emulators' tap0 and checks every echo), `udpsum.py PCAP`,
  `dmesg-lines.py FILE...` (kernel log lines recorded per case). `MOD=dmtx74-1 m9-rmmod.sh a|b` for
  a module without ionic_rdma.
- **XDP programs.** `T/src/xdp/xdp_echo.c` -> `T/bin/xdp/xdp_*.o` (built on the host with clang 18,
  no libbpf headers; the guests have no clang). Load: `ip link set dev rocm-ernic0 xdpdrv obj
  /tmp/xdp_tx.o sec xdp` (`sec xdp.frags` for the `_frags` objects, which jumbo MTU needs); remove:
  `ip link set dev rocm-ernic0 xdp off`. `T/src/xdp/udp_echo_check.py DST SECS SIZE [PPS]` sends
  UDP to port 7777 and verifies the echoes.
- **Things that bite.**
  - Mainline ionic corrupts every second multi-buffer XDP frame (frag offset bug, in v7.2.6 too);
    fix in `patches/xdp-frag-offset/`. Use distinct payloads when testing XDP: identical ones hide it.
  - With KASAN the live-binding unload prints a KASAN report before the oops; the guest stays up
    but is tainted and leaks the binding. Restart the stack.
  - Do not wait in a shell loop on `pgrep -f PATTERN` when the loop's own command line contains
    PATTERN (I did; it never ends). Wait on a marker line in the output file instead.

## 13. Added by the stack-1 recovery and the GPU dma-buf test (2026-11-19, afternoon)

Results: `notes/test-gpu-dmabuf.md` (the GPU test) and
`notes/real-hardware-runbook.md` (written, NOT RUN). Evidence `T/evidence/gpu/`,
`T/evidence/tcp-test-recovery-2026-11-19.txt`.

- **State.** Stack up, both guests on `7.2.15-devmem` with the kernel's own in-tree ionic,
  server still `T/ernic/tx-offloads-1/build/rocm-ernic`. Hash-verified 64 MiB socat transfer
  passes both ways over `10.213.100.1N`. Section 11's F9 still holds (re-checked:
  `ionic-devmem-rx_l2` = 192.168.96.0/20, `ionic-devmem_l2` = 192.168.192.0/20), so
  **192.168.100.x between stack 1's guests is still broken**; `T/scripts/tcp-test-alt.sh [MB]`
  is `tcp-test.sh` over the 10.213.100.1N addresses (adds them if missing).
- **Why devmem-1 was lost overnight, and what it was NOT.** The `7.4.0-rc3-devmem-kasan-p2p`
  kernel is **fine**: booted, ran, and switched back. The `/boot` partition (1023 MiB, separate
  from `/`) had room, the debs installed cleanly (`/var/log/dpkg.log`), and `grub.cfg` named the
  kernel and initrd correctly. The failure was in the **VM's warm reset**: the console ends at
  `shutdown[1]: Rebooting.` with **no SeaBIOS banner afterwards**, i.e. QEMU never got back to
  firmware. SeaBIOS does print to the serial console here, GRUB does not (`GRUB_TERMINAL=console`,
  `GRUB_TIMEOUT=0`), so "silence after Rebooting." means the reset itself, not the kernel.
  Not reproduced: three in-guest reboots afterwards all came back. Suspected one-off in the
  vfio-user reset path, plausibly involving the 256G rocjitsu BAR0 and
  `pci-hole64-size=1024G`. **Operational rule: if a guest does not answer after a reboot, check
  the console for a SeaBIOS banner before suspecting the kernel; a cold `stack.sh down && up`
  recovers it.**
- **Editing a guest disk offline** (what was used before the kernel was cleared of suspicion):
  `stack.sh down`, then as root `qemu-nbd --connect=/dev/nbd0 -f qcow2 T/images/devmem-1.qcow2`;
  `nbd0p1` is `/` and **`nbd0p13` is a separate `/boot`** (mount both, `/boot` second). `nbd`
  was not loaded on this host before and was unloaded again afterwards. `guestfish`/`virt-*`
  are not installed.
- **A GPU with `/dev/dri` is reachable after all**, on the **stock `7.2.6-070206-generic`**
  kernel, whose amdgpu 7.1.9 DKMS module is already built. It needs the rocjitsu module options
  that `ktool`'s `dmabuf_test.py:464` writes and that were missing here:
  `/etc/modprobe.d/amdgpu-rocjitsu.conf` with `options amdgpu emu_mode=1 fw_load_type=0
  discovery=2 ip_block_mask=0x7f vm_update_mode=3 gpu_recovery=0 vramlimit=1024`
  (plus `/lib/firmware/amdgpu/ip_discovery.bin`, already present). Without them the probe dies
  at the PSP trust-OS handshake with `-22`. The file was left in place in guest 1; amdgpu is
  still blacklisted on the kernel command line, so the devmem kernels are unaffected.
  **A failed amdgpu probe leaves the module unremovable** (`rmmod` segfaults, "Device or
  resource busy") — reboot instead of retrying.
- **devmem TX works on the stock 7.2.6 kernel too** (never tested before): `ionic-mod.sh
  build/load ernic-dmtx1 1` against it builds and loads `ionic.ko` (`ionic_rdma.ko` does not
  load there — symbol versions), and a udmabuf devmem stream passes.
- **New programs/scripts.** `T/src/gpu/devmem_tx_gpu.c` = `devmem_tx.c` plus `-G <render node>`
  (amdgpu BO instead of udmabuf; `-J` for GTT). Build it **in the guest** (it needs
  `/usr/include/drm`). **Do not pass `-F` with `-G`**: pagemap has no PFN for a VRAM mapping and
  the program exits with `page 0 not present`.

## 14. Added by the GPU-VRAM devmem TX run (2026-11-19, evening)

Results: `notes/test-gpu-vram.md`. Evidence `T/evidence/gpu-vram/`.

- **Outcome: VRAM placement achieved.** A GPU dma-buf stayed in VRAM across the
  devmem TX binding and every TX frag DMA address was inside the GPU's BAR0
  (`0x300_xxxxxxxx`); the emulator read it (`tx_dma_error` 0) and 5.6 GB
  verified at the peer. The old "the emulator cannot read a BAR" claim is
  retracted in `ernic-gpu-dmabuf.md`, `test-gpu-dmabuf.md` and
  `real-hardware-runbook.md`.
- **State.** Stack up (cold-cycled twice). **devmem-1 on
  `7.4.0-rc3-devmem-kasan-p2p`** with a **patched amdgpu loaded** and the
  patched in-tree ionic `dmtx74-1` loaded; devmem-2 on `7.2.15-devmem` with its
  in-tree ionic. Server unchanged (`T/ernic/tx-offloads-1/build/rocm-ernic`).
  `tcp-test-alt.sh 64` passes both ways (`T/evidence/gpu-vram/Z-tcp-test.txt`).
  Guest 1 is not in the default state: `install-kernel.sh 7.2.15 1` returns it.
- **amdgpu on 7.4-rc3 needs two test-bed patches** (`T/gpu/patches/`), both
  built into `T/gpu/amdgpu-74/` by `scripts/gpu/build-amdgpu-74.sh`; install the
  **stripped** copy `T/gpu/amdgpu-74-mod/amdgpu.ko` (83 MB; the build artefact
  is 830 MB) over
  `/lib/modules/<rel>/kernel/drivers/gpu/drm/amd/amdgpu/amdgpu.ko`, `depmod -a`,
  `modprobe amdgpu`. Patch 0001 (no-VBIOS atom oops) is **confirmed still
  needed with the rocjitsu module options set**; patch 0002 defines
  `HAVE_ACPI_DEV_GET_FIRST_MATCH_DEV`, a DKMS configure macro that the kernel
  tree never defines, without which `gmc_v12_0_sw_init()` skips
  `amdgpu_gmc_init_mem_ranges()` and `gfx_v12_1` hw_init fails with
  `num_mem_partitions == 0`.
- **`modprobe.blacklist=amdgpu` does not block an explicit `modprobe`** — it
  only stops autoloading. Leave it on the command line so a boot is safe, and
  load amdgpu by hand.
- **The warm-reset failure is reproducible on the `-p2p` kernel.** An in-guest
  reboot of devmem-1 on `7.4.0-rc3-devmem-kasan-p2p` ends at
  `shutdown[1]: Rebooting.` with no SeaBIOS banner; `stack.sh down && up`
  recovers it. Both known occurrences are on that kernel (the only one that
  registers the 256 G GPU BAR0 with ZONE_DEVICE/p2pdma). **On this kernel, do
  not reboot a guest — cold-cycle the stack.**
- **`pci_p2pdma_distance()` succeeds only because the host CPU is AMD Zen.**
  The two functions sit under different QEMU root ports and QEMU's `8086:29c0`
  host bridge is not in `pci_p2pdma_whitelist[]`; `cpu_supports_p2pdma()`
  (AMD family ≥ 0x17) is what rescues it. On an Intel host this test would fall
  back to GTT.
- **New scripts.** `scripts/gpu/p2p-probe.bt` (p2pdma distance, attach
  `peer2peer`, pin, which sgt path, and `ionic_tx_map_frag` return bucketed by
  `addr >> 32`) and `scripts/gpu/run-vram-test.sh LABEL PORT SECONDS [args]`
  (one case end to end into `T/evidence/gpu-vram/`). bpftrace must probe
  `ionic_tx_map_frag.constprop.0`, not the plain name.

## 15. Added by the runtime test of the two carried ionic fixes (2026-11-19, night)

Results: `notes/test-ionic-fixes-7.2.6.md`. Scripts and evidence: `/var/tmp/ionic-devmem/overnight/a/`.

- **State.** Stack up (cold-cycled twice). **Both guests on `7.2.15-devmem`** (GRUB default on
  both; guest 1 was moved off the `-p2p` kernel by rewriting `zz-devmem.cfg` and cold-cycling,
  not by `install-kernel.sh`, which warm-reboots). Loaded on both: `fix726-shipped` = v7.2.6 +
  `srq/0001..0011` + `p4` from rocm-ernic HEAD `2d3a3d02`, with `ionic_rdma`
  (`overnight/a/scripts/mod.sh load fix726-shipped`; not persistent). `fix726-nofix` (without
  `srq/0010`, `0011`) is built in both guests too. 10.213.100.1N/24 added; server unchanged.
- **Things that bite.**
  - **Bind a test socket to the device (`so-bindtodevice=rocm-ernic0`) if the test takes the
    netdev down.** Otherwise the next TCP segment leaves by the default route on the SLIRP NIC
    and QEMU's user network resets the connection.
  - **RDMA works only over the 192.168.100.1N GIDs.** The emulator's mesh does not resolve the
    GID of the hand-added 10.213.100.1N address (`no mesh node owns destination gid`), so
    `rping -a 10.213.100.1N` fails; `rping -a 192.168.100.1N` works although IP to that address
    does not.
  - bash process substitution inside `nohup sh -c '…'` is a syntax error (dash): use `bash -c`.
  - Page pools: `sudo python3 /mnt/hostfs/linux-7.2.15/tools/net/ynl/pyynl/cli.py --spec
    /mnt/hostfs/linux-7.2.15/Documentation/netlink/specs/netdev.yaml --dump page-pool-get`.
