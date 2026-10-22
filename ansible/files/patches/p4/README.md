# ionic patches for the P4 control surface

One patch against the upstream Linux ionic Ethernet driver, giving a guest
an auxiliary device to bind a P4 control driver to.

## Base

Generated against **`v7.2.4`** of
`https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git`, which is
the ref `IONIC_KERNEL_REF` pins in
[`cmake/ErnicKernelModule.cmake`](../../../../cmake/ErnicKernelModule.cmake). That
is the same base the series in [`../`](../) uses, and the two are
applied to one tree, base series first.

| File | Touches | Upstream? |
| --- | --- | --- |
| `0001-ionic-register-a-P4-auxiliary-device.patch` | `ionic_if.h`, `ionic_lif.h`, `ionic_aux.c` | **No** — local |

## Why it exists

`ionic_auxbus_register()` creates exactly one auxiliary device, named
`rdma`, held in the single slot `lif->ionic_adev` and gated on
`IONIC_LIF_CAP_RDMA`. A driver that wants to speak to the P4 pipeline has
nothing to bind to.

The patch factors that function's body into a helper taking a name, a
capability bit and a slot, then calls it twice: once for `rdma`, unchanged,
and once for a new `p4` device gated on `IONIC_LIF_CAP_P4`. Call sites do
not change — `ionic_auxbus_register()` and `ionic_auxbus_unregister()` keep
their signatures and their three callers each.

`enum ionic_lif_capability` upstream defines only `IONIC_LIF_CAP_ETH`
(bit 0) and `IONIC_LIF_CAP_RDMA` (bit 1), so bit 2 is free. Real hardware
never sets it, so a card running a kernel carrying this patch behaves
exactly as it does without it: `ionic_auxbus_add_one()` returns 0 without
allocating anything when the capability is absent.

rocm-ernic advertises the bit in `LIF_IDENTIFY` only when the P4 engine is
enabled **and** the operator granted the guest a scope with
`--p4-guest-scope`, so the device appears exactly when there is a control
surface behind it. See [`docs/p4.rst`](../../../../docs/p4.rst).

A failure to add the P4 device is warned about and swallowed rather than
returned. The two devices are independent, and failing the whole
registration because the optional one could not be created would turn a
missing extra into an RDMA regression.

## Applying it

The series is **optional** — only a guest that will talk to the P4 engine
needs it — so it is not in `patches/` where
[`fetch-ionic-sources.sh`](../../scripts/fetch-ionic-sources.sh) picks the
base series up by default. Name both:

```bash
ansible/files/scripts/fetch-ionic-sources.sh \
    --source-dir /path/to/ionic-src \
    --kernel-ref v7.2.4 \
    --patches-dir ansible/files/patches \
    --patches-dir ansible/files/patches/p4
```

`--patches-dir` may be repeated and the series are applied in the order
given. Giving it at all replaces the default, so the base series has to be
named too.

The script's sentinel covers the directory names as well as the patch
contents, so dropping the p4 series from a later run is seen as a change and
the tree is rebuilt rather than reported as already up to date.

## Retiring this directory

Delete it when either:

- the capability bit and the second auxiliary device land upstream, in
  which case the base ref moves past them and this patch stops applying; or
- rocm-ernic stops offering a guest-facing P4 control surface, in which
  case `--p4-guest-scope` goes too and nothing would bind to the device.

Renumbering is a re-test: the number is the order within this directory,
and the base series is always applied first regardless.
