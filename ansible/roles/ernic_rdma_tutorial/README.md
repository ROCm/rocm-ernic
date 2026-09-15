# ernic_rdma_tutorial

Runs selected examples from
[jcxue/RDMA-Tutorial](https://github.com/jcxue/RDMA-Tutorial)
across two already-provisioned rocm-ernic guest VMs.

## Overview

The upstream tutorial organises its examples as git commits rather than as
separate directories. This role clones the tutorial on each VM, checks out the
selected commits, builds the `rdma-tutorial` binary, then runs a server on VM1
and the matching client on VM2.

By default it covers:

- Example 1 (`65893ec`): RC send/recv
- Example 2 (`f73736e`): RDMA write

To match the upstream hostname-based role detection, the role temporarily
renames the two guests to `saguaro1` and `saguaro2` and adds matching
`/etc/hosts` aliases for the RDMA NIC addresses while the examples run.

## Requirements

- Two rocm-ernic VMs with RDMA already working
- `git`, `make`, `gcc`, `libibverbs`, and `librdmacm` available in the guests
- `become` is not required for the role itself, but passwordless `sudo` is
  required on the guests so their hostnames and `/etc/hosts` entries can be
  adjusted temporarily

## Role Variables

```yaml
ernic_rdma_tutorial_tests: true
ernic_rdma_tutorial_repo: https://github.com/jcxue/RDMA-Tutorial.git
ernic_rdma_tutorial_dir: /tmp/rdma-tutorial
ernic_rdma_tutorial_sock_port_base: 18600
ernic_rdma_tutorial_timeout: 60
ernic_rdma_tutorial_server_ip: "{{ ernic_nic_subnet }}.10"
ernic_rdma_tutorial_client_ip: "{{ ernic_nic_subnet }}.20"
ernic_rdma_tutorial_examples:
  - name: send-recv
    commit: 65893ec63c9eb004c310e25112c73a80d026c2c3
    msg_size: 64
    num_concurr_msgs: 1
  - name: write
    commit: f73736ed7a7d496f9acfc899e5d1997e791718ed
    msg_size: 64
    num_concurr_msgs: 1
```

Set `ernic_rdma_tutorial_examples` to a smaller or larger list to choose which
tutorial revisions run.

## Example

```yaml
- hosts: localhost
  gather_facts: false
  roles:
    - role: sbates130272.rocm_ernic.ernic_rdma_tutorial
      vars:
        ernic_vm_ssh_user: ubuntu
        ernic_vm_ssh_base_port: 2250
```

## License

MIT
