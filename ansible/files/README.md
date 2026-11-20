<!--
Copyright (c) Advanced Micro Devices, Inc. All rights reserved.

SPDX-License-Identifier: MIT
-->

# Collection files

The ionic driver patch series and the two scripts that build them, shipped
inside the collection so that a consumer who installs `rocm.ernic` gets
everything the SRQ roles need from one artifact at one ref.

```text
patches/README.md     what the base series is, and when to drop each patch
patches/0002-ionic-allocate-an-address-handle-for-UC-queue-pairs.patch
patches/0003-ionic-use-the-Rx-buffer-s-page-offset-for-XDP-frags.patch
patches/0004-ionic-free-every-temporary-queue-after-a-queue-count.patch
patches/srq/          the SRQ series, applied to v7.2.6, plus README.md.
                      It carries copies of the base series' own patches as
                      0009-0011, because --patches-dir replaces the default
                      directory rather than adding to it
patches/devmem-tx/    the optional netmem (devmem TCP) Tx series, applied
                      to v7.2.6 after the others, plus README.md.  Its
                      on-devmem-rx/ subdirectory holds the frag-limit
                      patch renumbered for a tree that also has an Rx
                      series; nothing applies a subdirectory
patches/devmem-rx-7.2.6/
                      the devmem TCP Rx series backported to v7.2.6,
                      applied after srq, p4 and devmem-tx 0001-0007,
                      plus README.md
patches/devmem-tx-mainline/
                      the same Tx series as it would be posted upstream,
                      for reference only: it is against v7.4-rc3, so
                      nothing here applies it, plus README.md
patches/devmem-rx/    the devmem TCP Rx series in its v7.4-rc3 form, for
                      reference only: it is on top of devmem-tx-mainline,
                      so nothing here applies it, plus README.md
patches/rdma-core/    linux-rdma/rdma-core#1799 plus two local fixes and
                      README.md
scripts/fetch-ionic-sources.sh
scripts/setup-ionic-dkms.sh
```

## Why these are here and not under a role

Three roles read this tree, not one:

* `ernic_srq_dkms` — `scripts/` and `patches/srq`
* `ernic_srq_provider` — `patches/rdma-core`
* `ernic_guest_setup` — `scripts/` and `patches/`, staged into a guest by
  `tasks/stage_sources.yml`

Putting it under whichever role you declare the owner would make the other
two reach across with `{{ role_path }}/../<that role>/files/...`. Here each
of the three says `{{ role_path }}/../../files/...`, which is the same
expression in all three and names no owner.

They used to live at the repository root, outside the collection, which
forced every consumer to clone rocm-ernic a second time through
`ernic_source` and pin that clone separately — so the roles came from one
ref and the patches they applied came from another, with nothing enforcing
that the two agreed. A git install of the collection builds it from
`ansible/` at the named SHA, so with the files in here the roles and their
inputs cannot disagree.

## Keep `scripts/` and `patches/` siblings

`fetch-ionic-sources.sh` defaults `PATCHES_DIR` to
`${SCRIPT_DIR}/../patches`. That default is what the in-VM driver installer
and anyone running the script by hand get, so moving either directory
without the other silently changes which series is applied.

## Shipping is asserted, not assumed

`build_ignore` in [`galaxy.yml`](../galaxy.yml) is a denylist, and its
patterns are matched with `fnmatch` against the path relative to the
collection root — where `*` crosses `/`, so a pattern as innocuous as
`*.sh` would drop `scripts/` from the artifact and from a `type: git`
install alike. The build job in
[`ernic-ansible-collection.yml`](../../.github/workflows/ernic-ansible-collection.yml)
therefore asserts that this tree is present, complete and executable in the
built tarball. A collection that silently stopped shipping it would put
every consumer back to a guest reporting `max_srq = 0`.

## The in-repo consumers

The CMake ionic targets
([`cmake/ErnicKernelModule.cmake`](../../cmake/ErnicKernelModule.cmake)) and
the install rules ([`cmake/ErnicInstall.cmake`](../../cmake/ErnicInstall.cmake))
read these paths directly. The installed layout under
`share/rocm-ernic` is unchanged, so the driver pack and the in-VM installer
see what they always did.
