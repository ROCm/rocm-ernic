# qemu-tool v2.1.0 and its ROCm 10.2 guest: what the stack provides

READ-ONLY study, 2026-11-19. Nothing was booted. Every command in section 7 is
**NOT RUN** unless it says otherwise.

Citations: `Q:` = `/var/tmp/ionic-devmem/qemu-minimal-2.1` (tag `v2.1.0`, commit
`c32dc74`); `R:` = `~/Projects/rocm-ernic` (feat/devmem-poc,
`2d3a3d02`). Evidence gathered here: `/var/tmp/ionic-devmem/test-rocm102/evidence/study/`
(`E:` below).

What was actually executed for this study, and nothing else:

- `oras manifest fetch` / `oras discover` / `oras blob fetch` of the image's manifest,
  referrer list and `vm-info.json` (metadata only, no disk pull): `E:image-manifest.json`,
  `E:image-referrers.json`, `E:vm-info-manifest.json`, `E:vm-info.json`.
- Two throwaway `docker run hello-world` probes (`E:docker-attached-probe.txt`).
- One read-only ssh into stack 1's guest `devmem-1` (port 2261) to read
  `/boot/config-7.2.6-070206-generic` and list `/usr/src` (`E:old-image-guest-readonly.txt`).
  That guest is from the **older** image `20261117.g481c892`, same kernel debs.
- `docker network ls/inspect`, `docker ps`, `ip route`, `df`, `ss` on the host.

## 0. Corrections to the brief, and the findings that change the plan

1. **There is no `fleet-quickstart.yml`.** The playbook is
   `Q:ansible/playbooks/vm-quickstart.yml` (`Q:AGENTS.md:22`,
   `Q:qemu/qemu-tool/quick_start.py:71-75`).
2. **No CI lane tests the NIC reading GPU memory.** The dma-buf RDMA test
   (`--rocm-rdma-test --rocm-rdma-mode dmabuf`) has only been run by hand
   (`Q:AGENTS.md:2719-2726`, `Q:AGENTS.md:2777-2780`). What CI exercises of P2P is the
   NVMe-to-GPU-BAR hipFile read in the daily lane, and the changelog says even that had
   not run in a lane at release (`Q:CHANGELOG.md:466`). Section 4.
3. **The image's own NIC server grants no Tx offloads**, so devmem TX bind fails with
   `EOPNOTSUPP` on the stack as shipped (`R:ansible/files/patches/devmem-tx/README.md`,
   "What the guest needs besides this"; `notes/testbed.md` section 5). The server must be
   swapped for a feat/devmem-poc build, and there is **no fleet element for a server
   binary or for extra server arguments** (`Q:qemu/qemu-tool/fleet_xml.py:196-285`).
4. **`quick-start test` cannot work on this host as it stands.** It reads the NIC byte
   counter with `docker exec ... awk` before and after the playbook
   (`Q:qemu/qemu-tool/quick_start.py:1507`, `:1515`, `:1685-1700`) and dies with "could
   not read the byte counter" when stdout is empty; and the playbook's first play pipes
   `docker run --rm ... | tar -xf -` on the controller to make `ip_discovery.bin`
   (`Q:ansible/playbooks/roles/rocjitsu_setup/tasks/firmware.yml:110-131`). Both host
   quirks were re-confirmed in this session (`E:docker-attached-probe.txt`: attached run
   with `--cgroup-parent` exits 0 and prints nothing; without it dockerd refuses).
   `qemu-tool list/inspect/ssh/down` use `docker exec` too
   (`Q:qemu/qemu-tool/shutdown.py:101`, `list_vms.py:744`, `inspect.py:1193`).
   Use the direct `ansible-playbook` form (7.4).
5. **The `l2` docker network has no subnet in the render** (`Q:qemu/qemu-tool/gen_compose.py:1554-1558`),
   so Docker picks one; that is how stack 2 landed on 192.168.96.0/20. There is no fleet
   element for it: pin it in `docker-compose.override.yml`. And the default
   `<fleet-prefix>` 172.31 (`Q:qemu/qemu-tool/fleet_xml.py:59`) is
   **in use by stack 2** (`ionic-devmem-rx_fleet 172.31.0.0/16`).
6. While `ionic-devmem-rx_l2` sits on 192.168.96.0/20, guest traffic on 192.168.100.x is
   masqueraded on every other stack's bridge (`notes/testbed.md` section 11). The image
   hard-codes 192.168.100 at boot (`Q:ansible/playbooks/roles/rocm_ernic_setup/defaults/main.yml:115`,
   template `rocm-ernic-netcfg.sh.j2` `SUBNET=`). So a third stack needs
   `<ernic-subnet>` outside 192.168.96.0/20 **and** the playbook's address step (or an
   `ip addr add` by hand) before guests can talk. Inferred from section 11, not re-tested.

## 1. The guest image

**Ref.** `docker.io/sbates130272/qemu-tool-rocm-golden:20261119.g2aea67d`
(`Q:qemu/stacks/rocm-golden.xml:80`). Manifest digest
`sha256:14b3785bee88...3396`; one zstd
layer of 1,546,691,156 bytes unpacking to a 7,993,294,848-byte qcow2 (64 GiB virtual);
annotations `vm.kernel_release=7.2.6-070206-generic`,
`vm.qemu_minimal_commit=2aea67d689e6...`, created 2026-11-19T21:02:53Z (`E:image-manifest.json`).
`gen-compose` refuses `latest`, `qm.<sha7>` and any malformed tag there
(`Q:qemu/qemu-tool/gen_compose.py:371-411`, `:413-470`).

**How it is pulled.** `oras`, not docker: artifact type
`application/vnd.batesste.vm-image.v1`, `oras pull` then `zstd`
(`Q:qemu/qemu-tool/gen_vm.py:311-343`). oras >= 1.3.4 (`quick_start.py:472`); the host
has 1.3.4. The first guest pulls to `<images>/<prefix>-1-backing.qcow2`, later guests
overlay that file (`quick_start.py:1059-1081`); a marker `…-backing.qcow2.image-ref`
records the ref (`quick_start.py:1003`). "`quick-start` has no way to be given a local
backing file" (`Q:AGENTS.md:2760-2768`, with the by-hand staging recipe).

**Credentials, and how quick-start reaches the guests.** Account `ubuntu`, a published default password,
passwordless sudo, sshd accepts both key and password; both are public
(`Q:qemu/stacks/rocm-golden.xml:36-48`, `:165-166`). The keypair is an OCI **referrer**
of the image (`application/vnd.batesste.vm-info.v1`: `vm-info.json`, `id_rsa`,
`id_rsa.pub`; `E:vm-info-manifest.json`). `quick-start` does `oras discover` + `oras
pull` into `--key-dir` (`quick_start.py:853-895`), copies it to
`<images>/keys/quickstart_id_rsa{,.pub}` (`:898-934`), adopts the account from
`vm-info.json` (`:822-850`) and logs in with
`ssh -i <key-dir>/id_rsa -p <ssh-port-base+N-1> ubuntu@127.0.0.1` (`:1272-1302`).
`gen-vm` (the non-quick-start route) puts the same pair at `<images>/keys/id_rsa`
(`gen_vm.py:377-423`). Ports bind to 127.0.0.1 (`rocm-golden.xml:49`).

**The stamp.** In a running guest: `/etc/rocm-golden-guest.json`
(`Q:ansible/playbooks/vm-rocm-golden.yml:1109-1180`). Companion records:
`/etc/ionic-srq-p4.json` (`:680-704`), `/etc/amdgpu-emu-patches.json`
(`roles/rocjitsu_setup/tasks/dkms.yml`), `/etc/perftest-rocm.json`,
`/etc/fio-hipfile.json`, `/etc/rocm-ernic-kernel-debs` (`roles/rocm_ernic_setup/tasks/kernel.yml:123-129`),
`~/WELCOME.md`. The same stamp is published as the `guest` object of `vm-info.json`, so
it can be read without booting; for this image (`E:vm-info.json`):

| Item | Value in `20261119.g2aea67d` |
| --- | --- |
| Ubuntu | resolute (26.04), amd64 |
| Kernel | `7.2.6-070206-generic`, from **Ubuntu's mainline PPA** `https://kernel.ubuntu.com/mainline/v7.2.6/amd64/`: `linux-image-unsigned-`, `linux-modules-`, `linux-headers-…-generic` and `linux-headers-…_all`, build `202609141300` (`roles/rocm_ernic_setup/defaults/main.yml:165`, `tasks/kernel.yml:42-142`; pin `ionic_kernel_pin: v7.2.6`, `vm-rocm-golden.yml:69`). GRUB pinned to it (`kernel.yml:156-193`); every other kernel purged (`vm-rocm-golden.yml:1209-1228`) |
| amdgpu | driver release **31.70** = `amdgpu-dkms 1:7.2.3.31700000-2428518.26.04`, DKMS module `amdgpu/7.2.3-2428518.26.04`, firmware package `1:31.70.0.0.31700000-2428518.26.04` (`vm-rocm-golden.yml:284`, `:292`) |
| amdgpu patches carried | `kfd_atomics`, `ras_vbios`, `panel_type`, `amd_vsdb_freesync`; **not needed: `kfd_ptrace_gate`** |
| GPU firmware | packaged gfx1250 blobs in `/lib/firmware/updates/amdgpu` plus generated gap set `gc_12_1_0_imu.bin`, `gc_12_1_0_mes.bin`, `gc_12_1_0_mes1.bin`; **`ip_discovery.bin` absent** (`ip_discovery_bin: false`) |
| ROCm | therock stream, line 10.2, `amdrocm-runtime-dev10.2 10.2.0-2`, prefix `/opt/rocm/core-10.2` (`/opt/rocm` resolves to it) |
| rocm-ernic sources | `ernic_source_pin 90434a237232275fc8002cb3a6b5121ba836c594` |
| rdma-core | archive 61.0-2ubuntu3 as runtime under `/usr`; **66.0 built from rocm-ernic's series under `/usr/local`**, which every tool loads (`/usr/local/lib/libibverbs.so.1.18.66.0`) |
| perftest | `sbates130272/perftest` `feat/rocm-data-validation-legacy` at `e68c74e9…`, in `/usr/local/bin` |
| fio | `fio-3.43` (`d2dcd7e0…`) with `libhipfile`, at `/opt/fio-hipfile/bin/fio` |
| SDXI | rocm-sdxi `dfd76f23…` |
| P4 | `/usr/local/bin/p4_fp8_client`, group `ernic-p4` |
| memlock | unlimited |

**amdgpu: what each patch does** (`Q:ansible/playbooks/roles/rocjitsu_setup/files/amdgpu-emu-patches.py`,
exact-string replacements on `/usr/src/amdgpu-*`):

- `kfd_atomics` (`:63-77`): `kfd->pci_atomic_requested = … || (amdgpu_emu_mode == 1)`;
  the vfio-user device cannot advertise PCIe atomics and KFD requires them.
- `ras_vbios` (`:80-99`): return early from `amdgpu_ras_query_ras_capablity_from_vbios()`
  when `adev->mode_info.atom_context` is NULL (device has `rombar=0`, no option ROM;
  otherwise the probe oopses in `amdgpu_atom_parse_data_header`).
- `kfd_ptrace_gate` (`:102-150`): only for amdgpu-dkms 7.1.9; 7.2.3 calls its own
  `kcl_task_ptrace_dumpable_denied()` so the script reports "not needed".
- `panel_type` (`:185-…`): separate configure probe for `display_info->panel_type` /
  `DRM_MODE_PANEL_TYPE_LCD` (an amdgpu build bug on mainline >= 6.19, not emulation).
- `amd_vsdb_freesync` (`:257-…`): same kind of bug, new in 7.2.3: guards four freesync
  members of `display_info.amd_vsdb` mainline does not have.

Build: `rocm_setup` installs the package with the postinst build deferred; the role
patches, then `dkms add/build/install amdgpu/<ver> -k $(uname -r) --force`
(`roles/rocjitsu_setup/tasks/dkms.yml`; `vm-rocm-golden.yml:285-292`).

**rocjitsu module options.** amdgpu is blacklisted three ways (modprobe.d, kernel
cmdline `modprobe.blacklist=amdgpu`, initramfs hook; `roles/rocjitsu_setup/tasks/amdgpu.yml`).
Nothing loads it at boot. `/usr/local/bin/amdgpu-probe` loads it with
`emu_mode=1 fw_load_type=0 discovery=2 ip_block_mask=0x7f vm_update_mode=3
gpu_recovery=0 vramlimit=1024` (`roles/rocjitsu_setup/files/amdgpu-probe`, `params=`),
refuses while `/lib/firmware/amdgpu/ip_discovery.bin` is missing, and
`amdgpu-probe --status` prints `absent|nofirmware|ready|loaded|unbound|stranded`.
Never `rmmod amdgpu` ("has been seen to GPF"). `ip_discovery.bin` is per rocjitsu build
and is installed live by `quick-start test` / `vm-rocm-golden.yml --tags firmware`, or
by hand with `rj-ip-discovery gfx1250 FILE` in the rocjitsu container
(`amdgpu-probe` `say_how_to_stage`).

**ROCm userspace.** Minimal therock install: `libstdc++-14-dev`,
`amdrocm-runtime-dev10.2`, `amdrocm-hipfile-dev10.2`, `amdrocm-blas-dev10.2` (headers
only) (`vm-rocm-golden.yml:239-252`), plus `amdrocm-amdsmi10.2`, `bpftrace`,
`hsa-snoop 1.3.0` (`roles/rocjitsu_setup/defaults/main.yml`, `rocjitsu_setup_tools_packages`).
`hipcc` at `/opt/rocm/bin/hipcc`. No amd-metrics-exporter, no rocm-cli, no RVS.

**The ionic driver.** rocm-ernic ref `90434a2` (same commit as the server image's
`ernic.90434a2`; `vm-rocm-golden.yml:104`, held by `tests/test_playbook_pins.py`).
Series: **`patches/srq` 0001-0009 (base patch included as 0009) + `patches/p4` 0001
renumbered 0010**, merged into one directory on the controller (`vm-rocm-golden.yml:414-439`),
on v7.2.6 sources fetched from `https://github.com/gregkh/linux.git` and checked
against commit `500df175a7f9…` (`:79`, `:87`). Built by **`rocm.ernic.ernic_srq_dkms`**
as the DKMS package **`ionic-ernic/7.2.6`** (`:449-466`), fetch on the controller, no
module reload. `ernic_p4.ko` by **`rocm.ernic.ernic_p4_dkms`** as DKMS `ernic-p4/0.2.0`
against a second fetch in the guest at `/usr/src/ionic-ernic-p4` with
`ernic_p4_dkms_patches_dirs: [srq, p4]` (`:597-614`). The collection (the roles) is
pinned separately at `bd902345` (`Q:ansible/requirements.yml`; `Q:AGENTS.md:1962-1975`).
The bake's first play also builds a base-only `ionic-ernic` through
`rocm.ernic.ernic_guest_setup` that the SRQ+P4 play replaces (`vm-rocm-golden.yml:173-212`).
Modules land in `/lib/modules/7.2.6-070206-generic/updates/dkms/{ionic,ionic_rdma,ernic_p4}.ko.zst`,
load from `modules-load.d`, and are kept out of the initramfs (`:637-643`). Record:
`/etc/ionic-srq-p4.json` with the three `srcversion`s (`:670-704`). `dkms status` shows
exactly `amdgpu`, `ernic-p4/0.2.0`, `ionic-ernic/7.2.6` (`Q:AGENTS.md:3300-3301`).
**This driver has neither the two base bug fixes of feat/devmem-poc (srq 0010/0011)
nor any devmem patch.**

**rdma-core / perftest / fio.** Provider by `rocm.ernic.ernic_srq_provider` from
`patches/rdma-core` into `/usr/local` (`vm-rocm-golden.yml:542-558`); archive `-dev`
packages purged (`:948-957`). Two perftests: archive's in `/usr/bin` (no ROCm; the plain
RDMA test names `/usr/bin/ib_send_bw`) and the ROCm one in `/usr/local/bin`
(`:815-852`). Two fios: archive's on PATH, hipFile one by path (`:867-898`).

**Also in the image.** gcc 15.2, make, cmake, ninja, dkms, git, kernel headers at
`/lib/modules/$(uname -r)/build` (`Q:qemu/checks/rocm-golden.sh:27-28`), iperf3,
ethtool not checked; `/var/tmp/ionic-src` (base-series sparse kernel checkout, kept by
`image_slim`, `roles/image_slim/defaults/main.yml:85-86`); naming rules
(`rocm-ernic0` / `rocm-rdma-ernic0`); boot unit `rocm-ernic-netcfg` (address
192.168.100.(10+n) from the MAC); `qemu-tool-hostname` unit; node-exporter :9100 and
rdma_exporter :9879; `/var/lib/shim-signed/mok/MOK.priv` (DKMS signing key,
`Q:AGENTS.md:2712-2714`). apt lists are removed: `sudo apt-get update` before any install.

## 2. The default fleet (`Q:qemu/stacks/rocm-golden.xml`)

**Shape.** 2 guests `rocm-golden-1/2` (`:29-30`), each: 1 ernic NIC, 1 rocjitsu
gfx1250 GPU, 1 SDXI device (`:84-86`), 1 NVMe (`:94`) whose `vram-dev` is the guest's
GPU BAR (`:100`, rendered as `--nvme-vram-dev rocjitsu-N`, `gen_compose.py:1283`),
vfio-user devices behind root ports (`:101`). 4 vCPU / 4096 MiB / shm 5g (`:89-92`);
`quick-start` resizes from the host to 4..8 vCPU and 4096..8192 MiB unless
`--no-size-from-host` (`quick_start.py:590-620`). P4 on with guest scope `load`
(`:120-121`). TAP on (`:115`). Grafana + Loki/Alloy on (`:135-136`).

**Images.**

| Element | Ref |
| --- | --- |
| `<qemu-ref>` (`:77`) | `docker.io/sbates130272/batesste-ci-images-ubuntu-qemu-libvfio-user-sbates-fork:20261112.gdca6903-qemu.4cb9698-vfu.3719c3a` |
| `<ernic-ref>` (`:78`) | `docker.io/sbates130272/batesste-ci-images-ubuntu-rocm-ernic-sbates-fork:20261112.gdca6903-ernic.90434a2-vfu.3719c3a` |
| `<rocjitsu-ref>` (`:79`) | `docker.io/sbates130272/batesste-ci-images-ubuntu-rocm-rocjitsu:20261112.gdca6903-rocjitsu.50be05a` |
| `<sdxi-image>` (`:81`) | `docker.io/sbates130272/batesste-ci-images-ubuntu-rocm-sdxi:20261113.g379fbf6-sdxi.dfd76f2` |
| tapsetup | `qemu-tool/ernic-tapsetup:1`, **built** by compose from `<stack>/tapsetup/` (alpine + iproute2; `gen_compose.py:72`) |

The first three are the same refs stacks 1 and 2 already run and are in the local
docker image store; the SDXI image is not and will be pulled. The fork QEMU carries
`a2ee87e` "hw/vfio: assign region fd to RAMBlock for vfio-user P2P DMA", which hands
each vfio-user server the other devices' regions; the NIC server then maps one
262144 MiB `/memfd:rocjitsu-vram` (`Q:AGENTS.md:3070-3115`).

**Networks in 2.1.0** (`Q:qemu/compose/rocm-golden/docker-compose.yml`, end of file;
`gen_compose.py:1040-1099`, `:1552-1572`):

- `fleet`: bridge, `<fleet-prefix>.0.0/16` (default 172.31), fixed addresses, third
  octet = family (0 infra, 1 ernic, 2 rocjitsu, 3 qemu, 4 sdxi; `gen_compose.py:59`).
  Carries the TCP mesh (`-b tcp:manager:listen:6320` on ernic-1,
  `tcp:worker:<ernic-1 addr>:6320` on the rest), RDMA payload only.
- `l2`: bridge, **no subnet given**. Guest Ethernet: each `ernic-N` runs
  `rocm-ernic … -T tap0`; sidecar `tapsetup-N` (`network_mode: service:ernic-N`,
  `NET_ADMIN`, exits at once) creates a persistent `tap0` owned by the container uid and
  bridges it to the container's `l2` interface in `br0`
  (`Q:qemu/compose/rocm-golden/tapsetup/setup.sh`). No host tap or bridge; no sudo.
- Guest addresses: MAC `72:6f:63:6d:00:NN`, IP `<ernic-subnet>.(10+N)` (default
  192.168.100), and `ERNIC_TCP_GUEST_GIDS=<link-local>,<ip>` per server
  (`gen_compose.py:620-630`, `:1073`). The boot unit in the image uses 192.168.100
  whatever `<ernic-subnet>` says; `quick-start test` adds the fleet's address
  (`vm-quickstart.yml:83-93`).

**Published ports.** Only `127.0.0.1:<ssh-port-base+N-1> -> 2222` per guest (2222,
2223) and Grafana `127.0.0.1:3000` (`Q:AGENTS.md:1641-1652`). Exporter forwards
(9100, 9879) are QEMU hostfwds inside the qemu container. Host port 3031 is in use here.

**Every override knob.**

| Want | How |
| --- | --- |
| Local overlay fleet | a `<fleet xmlns="http://qemu-tool/fleet/1.0">` file with only the changed elements, passed as a later `--xml`; later files win per element; parser is strict (`Q:AGENTS.md:117-134`). Table of every element: `fleet_xml.py:196-285` |
| Guest names | `<vm-name-prefix>` (also names the disk files) |
| Stack dir | `quick-start --stack-dir DIR`; `gen-compose --output-dir DIR` + `compose --stack-dir DIR` |
| **Compose project name** | not a setting: it is the **basename of the stack directory** (`quick_start.py:216-217`, `:1140`; `compose.py:659-742` runs `docker compose` with cwd there) |
| SSH ports | `<ssh-port-base>` (quick-start also moves off a taken port and records it in `overlay-ports.xml`, `quick_start.py:767-815`) |
| Grafana port / off | `<grafana-port>`, `<grafana-enable>false`, `<logs-enable>false`; `quick-start --no-metrics` |
| Mesh subnet | `<fleet-prefix>A.B` (a /16) |
| Guest Ethernet subnet | `<ernic-subnet>A.B.C` (see finding 6) |
| `l2` subnet / MTU | **no element**: `networks: l2: ipam/driver_opts` in `<stack>/docker-compose.override.yml`. `gen-compose` never touches that file (`gen_compose.py:1728-1741`) and `docker compose` merges it because neither wrapper passes `-f` |
| Mesh port | `<ernic-tcp-port>` (not published) |
| Images dir | `<vm-images-dir>` or `--images` |
| Key dir | `quick-start --key-dir` |
| Sizing | `<vm-vcpus>`, `<vm-vmem>`, `<vm-shm-size>` (>= vmem) and `--no-size-from-host` |
| Host directory into guests | `<vm-filesystem>/path` (9p; stock kernel has `9P_FS=m`) |
| Big-BAR placement | `<vm-cpu-phys-bits>`, `<vm-pci-hole64-size>`. Upstream says one 256 GiB BAR fits the machine default (`Q:AGENTS.md:1543-1549`); stack 1 on this host needed `42` / `1024G` (`test/stack/devmem-fleet.xml`). **Unresolved: check `lspci -vv` for BAR0 on first boot** |
| P4 / SDXI / NVMe off | `<p4-enable>false`, `<sdxi-per-vm>0`, `<vm-nvme>0` **with** `<vm-nvme-vram>false` (`gen_compose.py:288-323`) |
| qemu-tool inside the qemu containers | `<qemu-tool-src>`; quick-start copies `<repo-root>/qemu` into the stack dir and sets `QEMU_TOOL_SRC` (`quick_start.py:1132-1135`, `:1226`) |
| **Server image** | `<ernic-ref>local/name:tag`. A tag with no `<date>.g<sha>` stamp is warned about and excluded from the coherence check, so a locally built image renders (`gen_compose.py:535-548`). A stamped tag must match `<qemu-ref>`'s stamp and `vfu.<sha>` (`:549-572`). Must be built against libvfio-user `vfu.3719c3a`, i.e. FROM the pinned ernic image |
| **Server binary** | no element. Bind-mount over `/usr/local/bin/rocm-ernic` from the override (what `test/scripts/stack.sh server` does) |
| **Extra server arguments** (`--nic-profile header-split`, log level) | no element; the command line is rendered at `gen_compose.py:1074-1099`. Either mount a wrapper script at `/usr/local/bin/rocm-ernic` that execs the real binary with `"$@"` plus the extras (`stack.sh server-args`), or replace `entrypoint:` of each `ernic-N` in the override (keep the tap wait, the per-node `-s/-b/-m/-S`, and write `$$` for `$`) |
| cgroup parent (this host) | `cgroup_parent: system.slice` on every service, in the override |

Always restart the whole stack after changing a server: restarting `ernic-1` alone
wedges the guests (`gen_compose.py:1049-1052`, issue 7).

## 3. `quick-start`

`qemu-tool quick-start [options] [VERB]` (`Q:qemu/qemu-tool/cli.py:309-549`).

**Verbs** (`quick_start.py:63`, dispatch `:1977-2038`): `all` (default: preflight, up,
test, report, summary), `preflight` (checks tools docker/oras/ansible-playbook/git/ssh/
qemu-img, KVM, identity; touches nothing), `up` (preflight, port resolution, key fetch,
overlays, render, `docker compose up --detach`, wait for NIC servers healthy; does
**not** wait for guests), `test` (wait for ssh on each guest up to 900 s, run the
playbook, check the NIC counter, write the report), `report`, `down` (guest-agent
shutdown per guest by name, then `compose down`; overlays kept), `logs [-f]`.

**Options**: `--flavor default|mi355x|helios` (only `default` has a playbook),
`--xml FILE` (repeatable), `--clean-overlays`, `-f/--follow`, `--yes/-y`,
`--stack-dir`, `--key-dir`, `--images`, `--report-dir`, `--repo-root`,
`--[no-]size-from-host`, `--[no-]metrics`, `--in-network`, `--rdma-duration S`,
`--nvme-duration S`, `--sdxi-test`, `--p4-test`, `--p4-iters N`, `--hipfile-test`,
`--rocm-rdma-test`, `--rocm-rdma-mode host-alloc|dmabuf|vram`. The test switches are
ignored (with a note) by every verb but `test` and `all` (`quick_start.py:1963-1975`)
and refused up front when the fleet cannot run them (`:1338-1453`).

`up` and `test` are separate processes that agree through overlays written beside the
stack: `overlay-sized.xml`, `overlay-images.xml`, `overlay-ports.xml`,
`overlay-metrics.xml`, `overlay-image-login.xml` (`:423-424`). Pass the same `--xml`,
`--stack-dir`, `--key-dir` to every verb.

**What `test` runs**: `ansible-playbook -i <stack>/fleet-inventory.yml
playbooks/vm-quickstart.yml --private-key <key-dir>/id_rsa -e
quickstart_artifacts_dir=<stack>/artifacts -e ansible_remote_tmp=/tmp/.ansible/tmp`
plus one `-e` per switch, cwd `<repo-root>/ansible` (`quick_start.py:1456-1488`,
`:1509-1510`). Only repo-local roles are used (`rocjitsu_setup`, `rocm_ernic_setup`,
`sdxi_setup`), so no galaxy collection is needed for `test`.

| Test | Switch -> extra var | Play (`vm-quickstart.yml`) | Steps | Pass criterion | Needs in the guest |
| --- | --- | --- | --- | --- | --- |
| Configure | always | "Make both guests test-ready" `:37-232` | generate gfx1250 firmware incl. `ip_discovery.bin` with `docker run <rocjitsu image> vfio_guest_firmware.py` **on the controller** and install it; naming rules; `ip addr add <fleet ip>/24` on the NIC found by MAC `72:6f:63:6d`; `amdgpu-probe`; wait for a named KFD node (120 s); `ibv_devinfo -l`; wait `PORT_ACTIVE` | each task | `amdgpu-probe`, patched amdgpu, `/opt/rocm/bin/hipcc`, ionic_rdma loaded |
| Exporters | always | `:256-285` | node_exporter, rdma_exporter, SDXI textfile | non-fatal | |
| GPU | always | "Prove the GPU" `:289-354` | `rocm_agent_enumerator`; `hipcc --offload-arch=<gfx> vector_add.hip`; `vector_add 4096` | rc 0 and `vector-add: PASS` | hipcc, bound GPU |
| RDMA | always (`--rdma-duration`) | `:364-447` | last guest: `systemd-run /usr/bin/ib_send_bw -d <dev> --iters 1000 --size 65536`; first guest: same with the peer's fleet IP | a result row for 65536; **and** quick-start requires `total_bytes_sent` of ernic 1 to rise (`quick_start.py:1515-1521`) | archive perftest, L2 between guests |
| NVMe | always (`--nvme-duration`) | `:452-516` | `fio` seq write then read on raw `/dev/nvme0n1`, 128k, direct, 10 s | both bandwidths > 0 | archive fio |
| SDXI | `--sdxi-test` -> `quickstart_sdxi_test=true` | `:533-640`, every guest | 4 MiB of urandom, `sdxi-cli -d /dev/sdxi0 copy src dst`, `cmp` | identical | `sdxi-cli`, `sdxi_char`, `sdxi` group |
| P4 | `--p4-test` (`--p4-iters`) -> `quickstart_p4_test`, `quickstart_p4_iters` | `:681-860`, first guest only | `RDMA_DEVICE=<dev> p4_fp8_client /usr/local/share/rocm-ernic/fp8_policy.json`; `p4_fp8_bench <dev> 262144 <iters>` | client rc 0, bench rate > 0 | `ernic_p4`, `/dev/ernic-p4` (fleet `p4-enable` + scope `load`), group `ernic-p4`. **Leaves the fp8 program in NIC 1 until its server restarts** |
| hipFile | `--hipfile-test` -> `quickstart_hipfile_test` | `:909-1300`, every guest | compile `gemm-hipfile-bench.hip` with hipcc; `mkfs.ext4 -F /dev/nvme0n1`; mount `/mnt/quickstart-hipfile`; check the mount is on an NVMe namespace; `gemm-hipfile-bench <mnt> 4`; write 64 MiB, drop caches, `/opt/fio-hipfile/bin/fio --ioengine=libhipfile --rocm_io=hipfile --rw=read --bs=1M --direct=1`; unmount | bench verified + rate > 0; fio rate > 0 | hipFile SDK, bound GPU, scratch NVMe (it is reformatted) |
| ROCm RDMA | `--rocm-rdma-test` -> `quickstart_rocm_rdma=true`; `--rocm-rdma-mode M` -> `quickstart_rocm_rdma_mode=M` | `:1602-2144`, **tag `rocm_rdma`** (`:1606`) | check `/etc/perftest-rocm.json` and `amdgpu-probe --status` on both; last guest: `systemd-run /usr/local/bin/ib_write_bw -d <dev> -p 18516 --use_rocm=0 [--use_rocm_dmabuf] -F --data_validation -t 128 -n 1000 -s 65536` (root); first guest the same with the peer IP, under `timeout 600` | client rc 0, `VALIDATION: PASSED` with no FAILED/WARNING/ERROR, `Use ROCm memory : ON` on both sides | ROCm perftest; GPU bound on both |

ROCm RDMA modes (`quick_start.py:122-134`; `vm-quickstart.yml:1616-1648`):
`host-alloc` (default) sets `PERFTEST_ROCM_HOST_ALLOC=1` (hipHostMalloc: host memory,
proves everything but the NIC reaching VRAM); `dmabuf` adds `--use_rocm_dmabuf`
(hipMalloc exported as a dma-buf, `ibv_reg_mr_ex`; passes on the fork QEMU only);
`vram` is `--use_rocm` alone and fails everywhere with `Couldn't allocate MR with
error=14`. Validation is host-staged by default (`PERFTEST_ROCM_VALIDATION=staged`);
`-e quickstart_rocm_rdma_validation=live` runs it on the GPU. `-e
quickstart_rocm_rdma_tool=ib_read_bw` is the other supported verb. A perftest killed
mid-run leaves the RDMA device broken until the stack is recreated (`:1575-1578`).

**Results.** Console recap; per-guest `<stack>/artifacts/<vm>.md` and
`<stack>/artifacts/rocm-rdma.md` (`quick_start.py:369-380`, `:1527-1550`); quick-start
greps them for the lines `sdxi copy: N bytes through … identical`, `p4 fp8 bench: …
MB/s`, `hipfile: read X MB/s, N bytes, verified`, `hipfile fio: …`, `rocm rdma:
pass|no verdict|known limitation|no server|fail, mode …` and fails if a requested test
left none (`:1602-1666`). `--report-dir DIR` writes `DIR/index.md` (`:1707-1748`).
Optional-test results also go to node-exporter textfiles for Grafana
(`vm-quickstart.yml:1443-1520`). Upstream's by-hand 10.2 figures: `Q:AGENTS.md:2719-2726`.

**One test against a stack that is already up.** `quick-start test --<flag>` always
re-runs the configure play and the three base tests first (about 1.5 to 2.5 min;
re-runnable). Only the ROCm RDMA play has a tag, so it alone can be run by itself:
`ansible-playbook … --tags rocm_rdma -e quickstart_rocm_rdma=true -e
quickstart_rocm_rdma_mode=dmabuf`. The other plays are untagged, and the facts play
(`:1303-1340`) needs `quickstart_ib_device` from the first play, so `--start-at-task`
at a later play fails there after the test has run. For a single SDXI/P4/hipFile run,
use the guest commands in the table by hand.

## 4. P2PDMA in CI

| Workflow | Runs | P2P between emulated devices? |
| --- | --- | --- |
| `qemu-minimal-report-for-vm-rocm-golden.yml` (daily 08:00 cron, push, dispatch; `:30-50`) | `quick-start up` then `quick-start test` on `qemu/stacks/rocm-golden.xml` unmodified, **no optional flag** (`:124-154`); `scripts/check-vm-devices.sh`; then four `scripts/generate-vm-report.sh` benches (`:316-384`) | **Yes, NVMe -> GPU BAR only**: `REPORT_ROCJITSU_BENCH` (guest 1: GEMM + `gemm-hipfile-bench <mnt> 4`, `generate-vm-report.sh:282-350`) and `REPORT_HIPFILE_FIO_BENCH` (guest 2: `scripts/vm-report/fio-hipfile-bench.sh`, fio `libhipfile` read). The P4 and SDXI benches are single-device |
| `qemu-minimal-quickstart-boot.yml` (every PR) | `quick-start up`, boot and device assertions | No |
| `qemu-minimal-ansible-playbook-test-vm-rocm-golden.yml` (PR) | bake, `qemu/checks/rocm-golden.sh`, one guest behind a stack with `<ernic-tap>false`, `<p4-enable>false`, `--tags configure,firmware`, driver check | No |
| `qemu-minimal-publish-qcow-rocm-golden.yml` | bake, guest check, same one-guest gate, `--tags test` (HIP workloads), `sdxi-cli copy` + `cmp` (`:375-461`) | No |

- **What the hipFile benches assert**: the bench prints `gemm-hipfile-bench: PASS`
  after comparing a GPU matrix product of data read through `hipFileRead` with a host
  product; fio must report a non-zero rate; the mount must be on an NVMe namespace.
  **Nothing asserts that the NVMe wrote into the GPU BAR instead of through QEMU's
  bounce path.** The only evidence is the rate (hundreds of MB/s against 11 to 13
  without `<vm-nvme-vram>`), and upstream says the cause is "inferred, not profiled"
  (`Q:qemu/stacks/rocm-golden.xml:95-99`, `Q:CHANGELOG.md:444-449`). At release the
  lane had not run with the setting (`Q:CHANGELOG.md:466`; `Q:AGENTS.md:6161-6170`).
- **NIC <-> GPU** (`ib_write_bw --use_rocm_dmabuf`): by hand only. The proof offered
  upstream is (a) payload: `VALIDATION: PASSED - 7 chunks, 58720256 bytes`; (b)
  `ernic-1`'s `/proc/<pid>/maps` shows one 262144 MiB `/memfd:rocjitsu-vram` region
  beside guest RAM on the fork and none on stock; (c) on stock QEMU the same command
  fails at the first work request with `remote access error (10)` and the server logs
  `could not gather 65536 bytes` (`Q:AGENTS.md:3107-3115`, `:5460-5519`). No
  per-descriptor address check of the kind `test-gpu-vram.md` did.
- **Relied on**: fork QEMU + fork-built ernic (`rocm-golden.xml:52-66`); guest kernel
  7.2.6 with `CONFIG_PCI_P2PDMA=y` (`Q:AGENTS.md:5489-5490`); amdgpu-dkms 7.2.3 with the
  probe options of section 1 (`vramlimit=1024`, `emu_mode=1`); GPU BAR0 256 GiB from
  rocjitsu's `gfx1250_mi455x.json` (`vram_aperture_bytes: 274877906944`,
  `Q:AGENTS.md:1509-1530`); ROCm perftest; for hipFile `<vm-nvme-vram>true`.

## 5. Rebuilding the guest's ionic with `devmem-tx` as the last series

**Role variables.**

- `ernic_srq_dkms` takes **one** directory, `ernic_srq_dkms_patches_dir`
  (`R:ansible/roles/ernic_srq_dkms/defaults/main.yml:65`); the bake stages a merged
  directory for it (`Q:vm-rocm-golden.yml:414-439`). For a re-bake, add a third copy
  loop there that renumbers `devmem-tx/*.patch` after the p4 patch. Note the staging
  comment assumes srq is 0001-0009 and renumbers p4 to 0010; in feat/devmem-poc srq
  has 0010 and 0011, so the p4 patch must become 0012 and devmem-tx 0013-0019.
- `ernic_p4_dkms_patches_dirs` is a list (`R:…/ernic_p4_dkms/defaults/main.yml:24-26`;
  bake passes `[srq, p4]`, `Q:vm-rocm-golden.yml:612-614`): append `…/patches/devmem-tx`
  so `ernic_p4.ko` is built against the same tree.
- `ernic_guest_ionic_extra_patches` (`R:…/ernic_guest_setup/defaults/main.yml:77`,
  used at `tasks/driver_ionic.yml:123`) only acts in `ernic_guest_setup`'s driver path,
  which always applies the **base** series first and rebuilds `ionic-ernic/7.2.6` from
  `/var/tmp/ionic-src`. The 2.1.0 configure stage turns that path off
  (`ernic_install_driver: false`, `Q:vm-rocm-golden.yml:1329`) because it replaced the
  baked SRQ+P4 driver; upstream rejected feeding it srq/p4 through this variable since
  base 0002 is srq/0009 and `git am` meets it twice (`Q:AGENTS.md:3377-3383`).
  **Do not use this variable on the published image.**
- Both need `ernic_source_pin` (and so the server) moved to a commit that has
  `patches/devmem-tx`; a re-bake is a new image, 30+ min.

**Can it be rebuilt in a running guest of the published image? Yes.** From the repo:
gcc/make/dkms/git and the headers are asserted (`Q:qemu/checks/rocm-golden.sh:27-28`),
and `image_slim` keeps DKMS `source/` trees, removing only `build/`
(`roles/image_slim/defaults/main.yml:35-41`, `:127-134`). Seen on the older image's
guest (`E:old-image-guest-readonly.txt`; **to be re-checked on 20261119.g2aea67d**):
`/usr/src/ionic-ernic-7.2.6/{Kbuild,dkms.conf,eth,rdma,uapi,Module.symvers}` (the
patched srq+p4 tree), `/usr/src/ernic-p4-0.2.0`, `/usr/src/ionic-ernic-p4` (a git
checkout), `/var/tmp/ionic-src` (base only), gcc 15.2.0, `/sys/kernel/btf/vmlinux`.
`pahole` is missing, so a module built in the guest has no BTF: probe it with kprobes,
not fentry.

**Least invasive procedure** (no DKMS change, not persistent; a reboot returns to the
baked driver; this is what `test/scripts/ionic-mod.sh` base `ernic-7.2.6` does and was
proven on stack 1):

1. Stage a copy of the image's DKMS source set (v7.2.6 ionic eth + rdma + uapi, +srq
   0001-0009 at `90434a2`, +p4) and apply `R:ansible/files/patches/devmem-tx/000[1-7]`
   on top on the host with `git apply` in a plain directory (the series only touches
   `ionic_txrx.c`, `ionic_dev.h`, `ionic_lif.[ch]`, `ionic_stats.c`).
2. In each guest: `make -C /lib/modules/$(uname -r)/build M=$HOME/ionic-mod/<name> modules`.
3. `sudo rmmod ernic_p4 ionic_rdma ionic; sudo insmod ionic.ko; sudo insmod
   ionic_rdma.ko`, then `sudo systemctl restart rocm-ernic-netcfg` (and re-add the
   fleet address). `ernic_p4` is loaded on this fleet and holds `ionic`
   (`Q:AGENTS.md:3288-3292`), so it must be unloaded first.
4. Caveats: `CONFIG_MODVERSIONS=y`. `ionic_rdma` is rebuilt in the same tree so it
   matches. The **baked `ernic_p4.ko` may refuse to load** against the new ionic
   ("disagrees about version of symbol"), since patch 0003 changes structs reachable
   from the exported functions' types: leave it unloaded (only `--p4-test` needs it) or
   rebuild `/usr/src/ernic-p4-0.2.0` against the new `Module.symvers`. NOT VERIFIED.
   The unsigned module taints the kernel but loads (`MODULE_SIG_FORCE` off, lockdown
   `FORCE_NONE`). `ernic-driver-check.sh` / `--tags configure` will (correctly) report
   the driver as replaced.

Persistent alternative: `fetch-ionic-sources.sh --kernel-ref v7.2.6 --patches-dir srq
--patches-dir p4 --patches-dir devmem-tx` on the host, copy the tree in, run
`setup-ionic-dkms.sh --source-dir D --kernel-ref v7.2.6` in the guest (replaces
`ionic-ernic/7.2.6`), rebuild `ernic-p4`, `update-initramfs`. More moving parts; not
recommended for a test stack.

The series is independent of the base/srq/p4 lines (its README, "Base"), so the baked
tree's lack of srq 0010/0011 does not block it; add those two if the fixes are wanted.

## 6. Does the 7.2.6 guest kernel have what devmem TX needs?

The kernel is Ubuntu's mainline PPA build of v7.2.6 (section 1). Its config is not in
the repo; the repo states only `PCI_P2PDMA=y`, `MODVERSIONS=y`,
`MODULE_SRCVERSION_ALL=y`, `IO_URING=y` (`Q:AGENTS.md:5489-5490`, `:3230-3232`;
`Q:qemu/checks/rocm-golden.sh:694-698`). Read from `/boot/config-7.2.6-070206-generic`
in a guest of the older image, which installs the same debs
(`E:old-image-guest-readonly.txt`):

| Need | Value |
| --- | --- |
| `NET_DEVMEM`, `PAGE_POOL`, `GENERIC_ALLOCATOR`, `DMA_SHARED_BUFFER` | y |
| `UDMABUF`, `MEMFD_CREATE`, `DMABUF_HEAPS(_SYSTEM)` | y |
| `PCI_P2PDMA`, `ZONE_DEVICE`, `DEVICE_PRIVATE`, `HMM_MIRROR`, `HSA_AMD_P2P` | y |
| `BPF_SYSCALL`, `BPF_JIT`, `BPF_EVENTS`, `DEBUG_INFO_BTF`, `DEBUG_INFO_BTF_MODULES` | y |
| `KPROBES`, `KPROBE_EVENTS`, `UPROBES`, `FPROBE`, `FTRACE`, `FUNCTION_TRACER`, `DYNAMIC_FTRACE` | y |
| `IONIC`, `INFINIBAND_IONIC`, `DRM_AMDGPU` | m (the DKMS builds in `updates/dkms` win) |
| `DMA_API_DEBUG`, `DEBUG_NET`, KASAN/lockdep | **not set** (the 7.2.15/7.4 test kernels had them) |
| `IKCONFIG` | not set: no `/proc/config.gz`, use `/boot/config-$(uname -r)` |
| `MODULE_SIG` y, `MODULE_SIG_FORCE` off, `LOCK_DOWN_KERNEL_FORCE_NONE` y | bpftrace kprobes and unsigned modules allowed |

`bpftrace` is installed in the image (`roles/rocjitsu_setup/defaults/main.yml`,
`rocjitsu_setup_tools_packages`). Check in a running r102 guest:

    grep -E '^(# )?CONFIG_(NET_DEVMEM|UDMABUF|PCI_P2PDMA|ZONE_DEVICE|DMA_SHARED_BUFFER|DEBUG_INFO_BTF|KPROBES|BPF_SYSCALL|FPROBE)[= ]' /boot/config-$(uname -r)
    ls /sys/kernel/btf/vmlinux /sys/kernel/btf/ionic; sudo bpftrace -l 'kprobe:ionic_start_xmit'
    cat /etc/rocm-golden-guest.json /etc/ionic-srq-p4.json; dkms status; ls /usr/src

Known limit carried over from `notes/test-gpu-dmabuf.md`: on v7.2.x the net core
attaches the dma-buf statically, so amdgpu pins it into GTT and the NIC reads system
pages; VRAM-in-place needs the v7.4 dynamic attach. The 10.2 image does not change
that (kernel still 7.2.6); amdgpu is now 7.2.3 instead of 7.1.9, which is untested for
this path.

## 7. Operator commands for a private copy (all NOT RUN)

Chosen, and why: project/stack dir `ionic-devmem-r102`; guests `r102-1/2`; SSH
**2281/2282**; mesh `<fleet-prefix>` **172.27** (172.17, 172.29, 172.31 are taken);
`l2` pinned to **10.102.0.0/24**; guest Ethernet `<ernic-subnet>` **10.102.100**
(10.102.100.11/.12), clear of 192.168.96.0/20, 192.168.240.0/20, 192.168.122.0/24,
the host LAN and stack 1's 10.213.100.x; no Grafana/Loki (host 3031 busy, fewer
containers).

### 7.1 Files

    B=/var/tmp/ionic-devmem/test-rocm102
    QM=/var/tmp/ionic-devmem/qemu-minimal-2.1
    QT="uvx --from $QM/qemu qemu-tool"
    STACK=$B/stack/ionic-devmem-r102
    X="--xml $QM/qemu/stacks/rocm-golden.xml --xml $B/stack/r102-fleet.xml"
    QS="--stack-dir $STACK --key-dir $B/key --repo-root $QM --no-metrics --no-size-from-host --yes"
    mkdir -p $STACK $B/images $B/key $B/evidence

`$B/stack/r102-fleet.xml`:

    <?xml version="1.0" encoding="UTF-8"?>
    <fleet xmlns="http://qemu-tool/fleet/1.0">
      <vm-name-prefix>r102</vm-name-prefix>
      <fleet-prefix>172.27</fleet-prefix>
      <ernic-subnet>10.102.100</ernic-subnet>
      <ssh-port-base>2281</ssh-port-base>
      <vm-vcpus>8</vm-vcpus>
      <vm-vmem>8192</vm-vmem>
      <vm-shm-size>9g</vm-shm-size>
      <vm-images-dir>/var/tmp/ionic-devmem/test-rocm102/images</vm-images-dir>
      <vm-filesystem>/var/tmp/ionic-devmem</vm-filesystem>
      <grafana-enable>false</grafana-enable>
      <logs-enable>false</logs-enable>
    </fleet>

`$STACK/docker-compose.override.yml`, written **before** `up` (host quirk 1 and the l2
subnet); add the two `volumes:` lines only when swapping the server (7.3):

    services:
      ernic-1:    {cgroup_parent: system.slice}
      ernic-2:    {cgroup_parent: system.slice}
      rocjitsu-1: {cgroup_parent: system.slice}
      rocjitsu-2: {cgroup_parent: system.slice}
      sdxi-1:     {cgroup_parent: system.slice}
      sdxi-2:     {cgroup_parent: system.slice}
      qemu-1:     {cgroup_parent: system.slice}
      qemu-2:     {cgroup_parent: system.slice}
      tapsetup-1: {cgroup_parent: system.slice}
      tapsetup-2: {cgroup_parent: system.slice}
    networks:
      l2:
        ipam:
          config:
            - subnet: 10.102.0.0/24

### 7.2 Bring-up and teardown

    $QT quick-start $X $QS preflight
    $QT quick-start $X $QS up            # oras pull (1.5 GB), overlays, render, compose up
    docker ps --filter label=com.docker.compose.project=ionic-devmem-r102
    ssh -i $B/key/id_rsa -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p 2281 ubuntu@127.0.0.1   # r102-1; 2282 = r102-2

Equivalent without quick-start (what stack 1 does):
`$QT gen-compose $X --output-dir $STACK`; `$QT gen-vm $X --vm-name r102-1`;
`$QT gen-vm $X --vm-name r102-2 --backing-file $B/images/r102-1-backing.qcow2`;
`$QT compose --stack-dir $STACK $X up -d` (key then at `$B/images/keys/id_rsa`).

Down: `$QT quick-start $X $QS down` will report a failed guest shutdown on this host
(docker exec has no stdout); power the guests off over ssh (`sudo poweroff`), wait for
the two qemu containers to exit, then `$QT compose --stack-dir $STACK $X down`.

### 7.3 Swapping the server

    # build inside the stack's ernic image (same image as stack 1, vfu.3719c3a);
    # use a COPY of test/scripts/build-ernic.sh with T=$B
    $B/scripts/build-ernic.sh poc 2d3a3d02        # -> $B/ernic/poc/build/rocm-ernic (git archive, no checkout writes)

Binary: in the override, under both `ernic-1` and `ernic-2`:

      volumes:
        - "/var/tmp/ionic-devmem/test-rocm102/ernic/poc/build/rocm-ernic:/usr/local/bin/rocm-ernic:ro"

Extra arguments (e.g. `--nic-profile header-split`): mount the binary at
`/opt/rocm-ernic-local/rocm-ernic` instead and mount a wrapper at
`/usr/local/bin/rocm-ernic`:

    #!/bin/sh
    exec /opt/rocm-ernic-local/rocm-ernic "$@" --nic-profile header-split

Image instead of a bind mount: `FROM <ernic-ref>` + `COPY rocm-ernic
/usr/local/bin/rocm-ernic`, tag it without a build stamp (e.g.
`local/rocm-ernic-devmem:poc`), set `<ernic-ref>` to it in the overlay, re-render.

Then restart the **whole** stack (guests off, `compose down`, `compose up -d`), and
confirm with `docker inspect ionic-devmem-r102-ernic-1-1 --format '{{json .Mounts}}'`
and `ethtool -k rocm-ernic0` in a guest (SG / tx-checksum / TSO on).

### 7.4 Tests

Official form (needs working `docker exec`/attached `docker run`; **expected to fail
on this host**, finding 4):

    $QT quick-start $X $QS test                                              # configure + GPU + RDMA + NVMe
    $QT quick-start $X $QS --sdxi-test test
    $QT quick-start $X $QS --p4-test test                                    # add --p4-iters N
    $QT quick-start $X $QS --hipfile-test test
    $QT quick-start $X $QS --rocm-rdma-test test                             # host-alloc
    $QT quick-start $X $QS --rocm-rdma-test --rocm-rdma-mode dmabuf test     # NIC reads VRAM dma-buf
    $QT quick-start $X $QS --rocm-rdma-test --rocm-rdma-mode vram test       # expected FAIL, error=14

Form for this host. First stage `ip_discovery.bin` by hand (the image has the rest):

    mkdir -p $B/fw
    id=$(docker create --cgroup-parent=system.slice --name ionic-devmem-r102-fw --user $(id -u):$(id -g) \
         -v $B/fw:/out --entrypoint /bin/sh \
         docker.io/sbates130272/batesste-ci-images-ubuntu-rocm-rocjitsu:20261112.gdca6903-rocjitsu.50be05a \
         -c 'python3 /usr/local/bin/vfio_guest_firmware.py --generation gfx1250 --set gap --output /out')
    docker start $id; docker wait $id; docker logs $id; docker rm $id
    # per guest: scp $B/fw/ip_discovery.bin, then
    #   sudo install -o root -g root -m 0644 ip_discovery.bin /lib/firmware/amdgpu/ip_discovery.bin

Then the playbook quick-start would have run, skipping the two controller-docker tasks:

    cd $QM/ansible
    A="ansible-playbook -i $STACK/fleet-inventory.yml playbooks/vm-quickstart.yml --private-key $B/key/id_rsa \
       -e quickstart_artifacts_dir=$STACK/artifacts -e ansible_remote_tmp=/tmp/.ansible/tmp"
    S='--start-at-task "Install the ernic udev naming rules"'
    eval $A $S                                                        # base: configure, GPU, RDMA, NVMe
    eval $A $S -e quickstart_sdxi_test=true
    eval $A $S -e quickstart_p4_test=true                             # -e quickstart_p4_iters=N
    eval $A $S -e quickstart_hipfile_test=true
    eval $A $S -e quickstart_rocm_rdma=true                           # host-alloc
    eval $A $S -e quickstart_rocm_rdma=true -e quickstart_rocm_rdma_mode=dmabuf
    $A --tags rocm_rdma -e quickstart_rocm_rdma=true -e quickstart_rocm_rdma_mode=dmabuf   # that test alone, stack already configured

Results in `$STACK/artifacts/*.md`. The NIC counter check quick-start adds is lost;
read it another way (e.g. `ethtool -S` / `rdma statistic` in the guest, or the stats
file through a one-off container on the `ionic-devmem-r102_ernic-stats` volume).

Order: run `--p4-test` last or restart the stack after it (the fp8 program stays in
NIC 1). Run the stock tests **before** swapping the guest's ionic, and again after.

## 8. Not established

- Nothing here was booted: every statement about a running `20261119.g2aea67d` guest
  comes from the repo, the published `vm-info.json`, or the older image's guest.
- Whether BAR0 (256 GiB) is assigned at the default phys-bits on this host (section 2).
- Whether the baked `ernic_p4.ko` loads against an ionic rebuilt with devmem-tx.
- Whether `--start-at-task` behaves as assumed with the `include_role` task name, and
  whether tapsetup's `pull_policy: build` succeeds for a third project on this host.
- Whether the ROCm `--use_rocm_dmabuf` RDMA path and the devmem TX path coexist in one
  boot of the emulated GPU.
