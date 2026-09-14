# Porting Dynamic Connection (DC) code to rocm_ernic

This note is for applications that today use **Mellanox DC** (`libmlx5`,
`mlx5dv_create_qp`, `mlx5dv_wr_set_dc_addr`).  **rocm_ernic is not wire- or
symbol-compatible with mlx5.**  Treat NVIDIA DC as a **semantic reference**
(ordering, roles, per-send destination) and port deliberately.

## Roles and data

| mlx5 / rdma-core concept | rocm_ernic equivalent |
| ------------------------ | --------------------- |
| DCT (`MLX5DV_DCTYPE_DCT`) | `rocm_ernic_dc_create_dct()` with SRQ + key |
| DCI (`MLX5DV_DCTYPE_DCI`) | `rocm_ernic_dc_create_dci()` |
| Per-send DC address | `rocm_ernic_dc_post_send()` (`remote_dctn`, `dc_access_key`, `ah_id`) |
| `dctn` from create | `rocm_ernic_dc_get_dctn()` on the DCT `ibv_qp` |

Public headers ship with the provider build under **infiniband** (for example
`rocm_ernic_dc.h`).  Kernel UAPI extensions live in [rocm_ernic-abi.h][abi].

## What not to expect

- No **binary** compatibility with `libmlx5` or `mlx5dv_*` entry points.
- No guarantee that host **ConnectX** DC traffic can be tunneled unchanged
  through the emulated device; the **verbs** backend is not in scope for DC
  pass-through unless added explicitly later.

## CI and tests

- **cmocka**: `ernic_dc_uapi` exercises UAPI struct sizes and opcode constants.
  It runs as part of the full `ctest` in
  [build-and-test.yml](../../.github/workflows/build-and-test.yml).

The provider-linked DC tests (`test_dc_loopback.c`, `test_dc_error_paths.c`)
have been removed. They linked **librocm_ernic**, the deprecated out-of-tree
provider, and were gated behind `ERNIC_RDMA_CORE_BUILD=OFF`, so they never
built in CI. `ernic_dc_uapi` above is the remaining DC coverage; it checks the
ABI rather than live traffic.

For NVIDIA’s high-level description of DC roles, see [NVIDIA DC QPs][nv-dc].

<!-- References -->

[abi]: ../../driver/rocm_ernic-abi.h

[nv-dc]: https://docs.nvidia.com/networking/display/rdmacore50/Dynamically+Connected+(DC)+QPs
