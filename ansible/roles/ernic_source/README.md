# ernic_source

Resolves the rocm-ernic source checkout that the other roles in this
collection copy from. It either uses a checkout you already have on the
Ansible controller, or clones one.

## Overview

`ernic_guest_setup` and `ernic_host_setup` need the driver sources, the
`rocm_ernic` rdma-core provider and the udev rules. Guests in a rocm-ernic
mesh usually have no route to GitHub, so the clone happens on the
**controller** and the files are pushed from there.

This role is included automatically by the roles that need it; you rarely
apply it directly.

## Role Variables

```yaml
# Use an existing checkout and skip the clone. The playbooks in the
# rocm-ernic repo set this to the repo root.
ernic_source_dir: ""

ernic_source_repo_url: "https://github.com/ROCm/rocm-ernic.git"
ernic_source_repo_version: "develop"
ernic_source_clone_dir: "/var/tmp/rocm-ernic-src"
ernic_source_force_clone: false
```

Pin `ernic_source_repo_version` to a commit SHA for any run you intend to
reproduce; `develop` moves. The only release tag on the repo today is
`ansible-v0.1.0`, so a SHA is the pin — not a version tag.

Pinning the collection does not pin this. The driver sources, the patch
series, `udev/99-rocm-ernic.rules` and the ionic kernel ref (`ernic_ionic_kernel_ref`
defaults to empty, meaning "read `IONIC_KERNEL_REF` out of the checkout's
`cmake/ErnicKernelModule.cmake`") all come from whatever this role resolved,
which is a different tree from the one the roles came from unless you say
otherwise. In this repo's own playbooks `ernic_source_dir` is set to the repo
root, so they never clone and never drift; a standalone consumer should pin
both and bump them together.

After the role runs, `ernic_source_dir` is set on every host in the play and
`ernic_source_resolved` is `true`, so re-including the role is a no-op.

## Example

```yaml
- hosts: image
  roles:
    - role: sbates130272.rocm_ernic.ernic_source
      vars:
        # A full SHA, matching the revision the collection came from.
        ernic_source_repo_version: "9329fbd494142bb57fdaa1993c84308873aebdee"
```

## License

MIT
