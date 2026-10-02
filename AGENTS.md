# Notes for agents

Known defects and traps in this tree. See `CLAUDE.md` for coding
standards, linters and copyright rules.

## Fixed: the TCP backend resolved every GID to the same node

Inter-node RDMA over the TCP mesh backend was broken above two nodes.
Every destination GID resolved to one node id, so traffic for nodes 2+
was delivered to node 0 (worker mode) or node 1 (everything else), with
no error anywhere.

### What was wrong

`tcp_resolve_node_from_gid()` derived a node id from one octet of the
destination GID by two narrow rules, and its callers substituted a
hardcoded default when neither matched:

```c
if (resolved != UINT32_MAX) {
    tqp->remote_node_id = resolved;
} else if (priv->mode == TCP_MODE_WORKER) {
    tqp->remote_node_id = 0;
} else {
    tqp->remote_node_id = 1;
}
```

The default was constant per instance and independent of the GID, which
is why one run showed three distinct GIDs producing one answer and two
instances disagreeing about which node that was.

Both rules were guesses. One read `raw[15]` as a node id, which only
holds for the synthetic GID built in `tcp_add_gid()` — and nothing calls
`rdma_rm_add_gid()`, so that GID is never registered. The other decoded
an IPv4-mapped GID under a `<subnet>.<(node_id + 1) * 10>` convention,
but [the DHCP pool][pool] allocated sequentially from `.10`, so only the
first node ever matched. The GIDs actually seen in the field were IPv6
link-local, which the first rule rejected outright.

### How it works now

Node identity is configured, not inferred. Each instance is told its own
guest RoCE addresses in `ERNIC_TCP_GUEST_GIDS` (comma-separated,
`inet_pton` notation; IPv4 is stored in the `::ffff:a.b.c.d` mapped form
[`adminq_parse_roce_hdr()`][parse] builds). It advertises them over the
mesh as `TCP_MSG_NODE_GIDS` once it knows its node id, the manager fans
each advertisement out to the rest, and peers keep a `GID -> node id`
table keyed by the full 16 bytes.

[`tcp_resolve_node_from_gid()`][resolve] is now nothing but an exact
lookup. A miss returns `UINT32_MAX` and the caller fails:
[`tcp_qp_state_rtr()`][rtr] returns `-EINVAL`, which
`rdma_rm_modify_qp()` turns into `-EIO`, and
[the adminq MODIFY_QP path][adminq] fails the command with the full GID
in the log. **Do not reintroduce a default peer**: a wrong node here is a
silent write into another guest's memory that nothing downstream catches.

Deployment side: `service/rocm-ernic-launcher` derives each instance's
IPv6 link-local GID from its MAC, which is what a guest autoconfigures.
Override with `ERNIC_GUEST_GIDS_<n>` when the guest also carries a static
address peers will target. An address not listed is unreachable.

Covered by `tests/test_tcp_gid_resolve.c`.

### Reproducing the original failure

Needs a minimum of three nodes. The `-T` TAP plus bridge setup is
required; without it the run never reaches the data path.

## Trap: the mesh has no Ethernet or DHCP path

Guest TCP/IP rides the host TAP/bridge ([`ionic_eth_net.c`][ethnet] is a
plain TAP open/send/recv). The mesh carries only `TCP_MSG_IONIC` plus
control traffic.

It used to also carry raw Ethernet frames and relay DHCP, but nothing
ever sent either — both were handled and never originated. That dead code
is gone, along with `src/net/dhcp_server.c` and `src/net/eth_rx_inject.c`.
Their message ordinals survive as `TCP_MSG_RESERVED_18..20`: the wire
protocol has no version negotiation beyond a magic number, so renumbering
what follows would silently misparse against an unupgraded peer.

If you need guest-visible networking, the TAP/bridge is the path. Do not
rebuild a second one inside the mesh.

## Adjacent issues

Observed alongside the GID bug, not yet diagnosed against the source:

- `CLOSE_WAIT` socket leak.
- An `EMFILE` retry loop with no backoff. One run produced 295 GB of
  logs.
- A registration wall at roughly 40 nodes. The number is measured on a
  64-node run; the full-mesh connection-cost mechanism behind it was
  inferred from socket counts and a slot-freeing experiment and is
  **not** confirmed against the code.
- Node ids are assigned monotonically, so they are not stable across
  runs.

Note that nodes are never evicted from `mesh_nodes` — a dead node is
marked `is_alive = false` and reconnects under the same id — so the GID
table has no removal path by design. A returning node re-advertises and
replaces its own entries.

[resolve]: src/rdma/rdma_backend_tcp.c
[rtr]: src/rdma/rdma_backend_tcp.c
[parse]: src/ionic_adminq.c
[adminq]: src/ionic_adminq.c
[pool]: src/rocm_ernic_compat.c
[ethnet]: src/ionic_eth_net.c
