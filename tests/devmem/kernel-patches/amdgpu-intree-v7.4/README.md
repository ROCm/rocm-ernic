<!--
Copyright (C) Advanced Micro Devices, Inc.
SPDX-License-Identifier: GPL-2.0-or-later
-->

# In-tree amdgpu on v7.4-rc3 with the emulated GPU

Two patches to the kernel's own `drivers/gpu/drm/amd` at `v7.4-rc3`,
without which it does not probe the emulated rocjitsu GPU (gfx1250).
The run that sent devmem TCP from GPU VRAM used an `amdgpu.ko` built
from the in-tree driver plus these two. Test aids; neither has been sent
upstream.

| Patch | Why |
| --- | --- |
| `0001-drm-amdgpu-ras-skip-vbios-query-without-atom-context.patch` | `amdgpu_ras_query_ras_capablity_from_vbios()` oopses in `amdgpu_atom_parse_data_header()` on a device with no VBIOS. The unpatched driver oopses there with the rocjitsu module options set. qemu-minimal carries the same fix for `amdgpu-dkms` as `ras_vbios`. |
| `0002-drm-amdgpu-define-HAVE_ACPI_DEV_GET_FIRST_MATCH_DEV-in-tree.patch` | `gmc_v12_0_sw_init()` and `soc_v1_0_get_xcp_mem_id()` are guarded by `HAVE_ACPI_DEV_GET_FIRST_MATCH_DEV`, a macro of the DKMS package's configure step that nothing in the kernel tree defines. An in-tree build leaves `num_mem_partitions` at 0 on GC 12.1 and the probe fails with "no compatible compute partition mode found". This looks like a bug of the `v7.4` tree, not of the test bed. |

Both apply with `patch -p1` to `v7.4-rc3`; they are not `git am`
files. `0002` was kept in the work area as a description only: its diff
here was taken from the tree the tested module was built from.

Built as an external module against the `7.4.0-rc3-devmem-kasan-p2p`
kernel by `scripts/stack1/gpu/build-amdgpu-74.sh`
(see [`../../README.md`](../../README.md)).

**Tested:** with both, amdgpu probes the emulated GPU on `v7.4-rc3`,
and a VRAM buffer it exports was bound for devmem Tx and sent from, every
byte verified at the receiver. `test-gpu-vram.md` in
[`docs/devmem/notes`](../../../../docs/devmem/notes) has the run.
**Not tested:** any real GPU.

The patches take the license of the files they change, which carry
the MIT notice amdgpu's sources do.
