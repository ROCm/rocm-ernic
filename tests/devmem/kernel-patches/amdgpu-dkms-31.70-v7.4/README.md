<!--
Copyright (C) Advanced Micro Devices, Inc.
SPDX-License-Identifier: GPL-2.0-or-later
-->

# amdgpu-dkms 31.70 on the devmem test kernels

Patches that let `amdgpu-dkms` 31.70 build against the `v7.4-rc3` kernel
the GPU-memory devmem tests ran on, and the script that built it. A test
aid for that kernel, not a supported port.

2026-11-19. Package: `amdgpu-dkms 1:7.2.3.31700000-2428518.26.04` from
`https://repo.radeon.com/amdgpu/31.70/ubuntu` (resolute), sha256
`7e83d6c4…e96f`, matching the apt index. `amdgpu/latest` points at it.

## Result

| Kernel | As shipped | With the patches |
| --- | --- | --- |
| v7.4-rc3 (`7.4.0-rc3-devmem-kasan-p2p`) | does not build | builds, 9 modules; with the modules built from 0001 to 0006 (0007 is a no-op there): probes the emulated GPU; devmem TX from VRAM verified |
| v7.2.15 (`7.2.15-devmem-p2p`) | does not build | with all seven: builds, 9 modules; **not loaded or run** |

Both built in the `devmem-kbuild:1` container (gcc 15.2) against the
unpacked `linux-headers` package of each kernel. The only warnings are the
package's own 12 `-Wmissing-prototypes` in `amd/amdkcl`.

Run on v7.4-rc3, guest 1 of stack 1, modules in `updates/dkms`:

- `modprobe amdgpu` returns 0: `[drm] Initialized amdgpu 3.65.0`,
  `/dev/kfd`, `added peer-to-peer DMA memory 0x30000000000-0x3003fffffff`.
- Devmem TCP transmit from a VRAM buffer, 30 s: 5,645,069,347 bytes, 0
  mismatches at the receiver; buffer `VRAM VISIBLE pin count 1`; attach
  peer-to-peer, `pci_p2pdma_distance` 4; 1,485,097 of 1,485,097 frag
  addresses at `0x300_xxxxxxxx` (GPU BAR0); emulator `tx_dma_error` 0.
  (Evidence `gpu-vram/D1-dkms3170-vram-*`, not in the repository.)
- Two lockdep reports during the probe, from the driver, not the patches:
  `gfx_v12_1_spm_set_interrupt_state()` takes `gfx_off_mutex` under the
  `irq.lock` spinlock ("sleeping function called from invalid context",
  "Invalid wait context").

## The patches

| Patch | Why | Needed on |
| --- | --- | --- |
| 0001 | v7.4 moved `drm_fb_helper.h` and renamed `drm_utils.h`. The build fails, and four configure tests fail silently, leaving six `HAVE_*` macros undefined. | v7.4 |
| 0002 | `shmem_writeout()` became `shmem_write_folio()` in v7.3; the fallback uses `->writepage`, which is gone. | v7.3+ |
| 0003 | The pre-v5.2 `drm_fb_helper_fill_info()` fallback is compiled on any kernel without `CONFIG_DRM_FBDEV_EMULATION`. | both (our kernels have it off) |
| 0004 | v7.4 `vga_client_register()` signature; `vga_switcheroo_client_ops.reprobe` removed. | v7.4 |
| 0005 | v7.4 removed the plane and CRTC `reset` callbacks for `atomic_create_state`. Ports four upstream commits. | v7.4 |
| 0006 | NULL dereference in `amdgpu_ras_init()` on a device with no VBIOS. Not a kernel-version change; the in-tree v7.4-rc3 driver has it too. | emulated GPU |
| 0007 | The driver reads `drm_amd_vsdb_info` members only v7.4 has, under a configure test that only checks the struct exists. | v7.0 to v7.3 |

0001 to 0005 are the port to v7.4. 0003 would be unnecessary on a kernel
with `CONFIG_DRM_FBDEV_EMULATION=y`, as distribution kernels have. 0005
does not convert the KUnit tests under `amd/display/amdgpu_dm/tests/`,
which are built only with `CONFIG_KUNIT`. 0002 and 0007 use kernel version
checks, which a distribution kernel with backports can defeat; the proper
fix for each is a configure test, but `configure` ships pre-generated.

## 0006 and 0007 are superseded

They are in [`superseded/`](superseded), not beside the port, because
qemu-minimal's guest provisioning already carries both fixes for the
emulated GPU, as `ras_vbios` and `amd_vsdb_freesync` in its
`amdgpu-emu-patches.py`, and does them as configure probes, which is
the proper form (see above on kernel version checks). Use those. The two
files are kept because the modules of the run above were built from
`0001` to `0006`, and the `v7.2.15` build needs `0007`.

## Reproduce

`build.sh` runs in the work area the tests used, not here (see
[`../../README.md`](../../README.md)): `scripts/deploy.sh` puts it and
the patches under `$DEVMEM_ROOT/amdgpu-31.70/`, where it expects the
unpacked `amdgpu-dkms` package in `x/` and builds in the `devmem-kbuild`
container.

```
scripts/build.sh LABEL KERNELVER HEADERS_DIR [PATCH...]
```

e.g. `scripts/build.sh x 7.4.0-rc3-devmem-kasan-p2p
headers/74p2p/usr/src/linux-headers-7.4.0-rc3-devmem-kasan-p2p patches/000*.patch`.
Logs are `build/LABEL/make.log`.

## Not done

- 31.70 on v7.2.15 was built, not loaded.
- 31.70 on the stock `7.2.6-070206-generic` kernel was not built with
  these patches; by the same struct layout it should fail without 0007.
- No ROCm or HIP workload: the run above sent from a GEM buffer exported
  with PRIME, and nothing else. It is the only record of that run; no
  note in [`docs/devmem/notes`](../../../../docs/devmem/notes) reports
  it.
- No `dkms install` in a guest; the modules were copied into
  `updates/dkms`.

The patches change files of `amdgpu-dkms` and take those files' licenses:
MIT for most, `GPL-2.0+` for `amdgpu_vkms.c`.
