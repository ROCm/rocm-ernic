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
ernic_source_repo_version: "main"
ernic_source_clone_dir: "/var/tmp/rocm-ernic-src"
ernic_source_force_clone: false
```

Pin `ernic_source_repo_version` to a tag or commit SHA when baking images you
intend to keep; `main` moves.

After the role runs, `ernic_source_dir` is set on every host in the play and
`ernic_source_resolved` is `true`, so re-including the role is a no-op.

## Example

```yaml
- hosts: image
  roles:
    - role: sbates130272.rocm_ernic.ernic_source
      vars:
        ernic_source_repo_version: "v0.2.0"
```

## License

MIT
