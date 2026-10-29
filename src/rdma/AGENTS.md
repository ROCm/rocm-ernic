# AGENTS.md for the RDMA backends and the TCP mesh

This adds to the repository's [AGENTS.md](../../AGENTS.md), which still
applies here; its Thread safety section matters most. The TCP backend's
locks and their order are in the `Locks` table at the top of
[`rdma_backend_tcp_priv.h`](rdma_backend_tcp_priv.h).

## GID resolution: never default a peer

The TCP backend once resolved every destination GID to one node (guessing
a node id from a GID octet, then falling back to a hardcoded node 0 or
1), silently sending traffic for nodes 2+ to the wrong node.

Node identity is now configured, not inferred. Each instance gets its
guest RoCE addresses in `ERNIC_TCP_GUEST_GIDS` (comma-separated,
`inet_pton` notation; IPv4 stored as `::ffff:a.b.c.d`, the form
`adminq_parse_roce_hdr()` in `src/ionic/ionic_adminq.c` builds) and, once
it knows its node id, advertises them as `TCP_MSG_NODE_GIDS`. The manager
fans these out, and peers keep a `GID -> node id` table keyed on all 16
bytes.

`tcp_resolve_node_from_gid()` in [`tcp_gid.c`](tcp_gid.c) is only an
exact lookup. A miss returns `UINT32_MAX`; `tcp_qp_state_rtr()` returns
`-EINVAL`, `rdma_rm_modify_qp()` turns it into `-EIO`, and the adminq
MODIFY_QP path fails the command, logging the full GID. **Never
reintroduce a default peer**: a wrong node is a silent write into another
guest's memory.

`service/rocm-ernic-launcher` derives each instance's IPv6 link-local GID
from its MAC (what the guest autoconfigures); override it with
`ERNIC_GUEST_GIDS_<n>` when peers target a static address. Unlisted
addresses are unreachable. Reproducing a failure needs three or more
nodes and the `-T` TAP plus bridge setup. Covered by
`tests/test_tcp_gid_resolve.c`.

## Dead and restarted nodes

A dead node stays in `mesh_nodes` with `is_alive = false`. A node that
went quiet comes back under its id (a worker that loses its connection
to the manager registers again asking for it, `tcp_register_again()`),
but a restarted one registers again and gets a new id (ids are
monotonic, so not stable across runs). Both ids then claim the same
GIDs.

So `TcpNodeGids` has an `alive` flag, cleared when the manager declares a
node dead and set when it answers or registers again, and
`tcp_gid_index_rebuild()` skips non-alive claimants: the restarted guest
owns its addresses under its new id, and a late node regains them
without re-advertising. Two *live* claimants still resolve to nothing.

- On the manager the flag copies `is_alive`: **change either only
  through `tcp_node_set_alive()`**, which writes both under
  `mesh_table_lock`. New claims take the node's `is_alive`
  (`tcp_gid_claims_set_if_member()`).
- Don't make the index read `is_alive` from `mesh_nodes`: that takes
  `mesh_table_lock` under `gid_table_lock`, the reverse of the lock
  order.
- A registration declares dead every node with no live connection
  (`tcp_declare_dead_if_unreachable()`), as the health check's next pass
  would, and does it **before it answers**
  (`tcp_register_declare_stale()`): the answer lets a restarted worker
  claim the GIDs its old id still holds. Only the broadcasts wait for the
  answer.
- Workers keep no liveness of their own: the manager advertises a dead
  node as owning nothing (`tcp_snapshot_node_gids()`), which is what
  gives an empty advertisement meaning.

## Broadcasts

All of the manager's broadcasts come from one thread,
`tcp_broadcast_thread()`. Handlers only ask for them
(`tcp_broadcast_topology()`, `tcp_broadcast_node()`,
`tcp_broadcast_node_gids()`, `tcp_broadcast_gid_table_to()`); a pass
sends a changed node's set to every worker, and the whole GID table only
to a node that has just registered.

**Don't send a topology or a GID set from anywhere else**: pruning
against the topology (`tcp_gid_claims_prune()`) is safe only because one
thread sends the snapshots in the order it took them. Ask only once the
answer to a registration has gone, since a worker ignores what comes
before it.

## Eviction

The broadcasts carry at most `TCP_MAX_MESH_NODES` (64) nodes, so when a
new node registers in a full table, the manager evicts the node that has
been dead longest and has no live connection (`tcp_evict_dead_node()`),
drops its claims, and advertises it as owning nothing. If none can be
evicted, it refuses the new node with `-ENOSPC`, so the table never holds
more than the broadcasts carry. An evicted id is never given out again,
and nothing is evicted once teardown has begun (`priv->stopping`, which
`tcp_register_again()` also checks).

- **Never keep a `MeshNodeInfo *` after dropping `mesh_table_lock`**:
  eviction frees it. Keep the node id and look it up again, as
  `TcpHealthCheckItem` does.
- A receive thread can outlive its connection's place in the table, so
  the manager records a worker's advertisement only while its node is in
  `mesh_nodes`, checked under that lock
  (`tcp_gid_claims_set_if_member()`); otherwise a late one would put an
  evicted node's claims back.
- A worker cut off from the manager misses the eviction's empty
  advertisement, and later GID tables don't mention the node, so a
  worker drops the claims of every node a topology update from the
  manager leaves out (`tcp_gid_claims_prune()`).

Liveness and eviction are covered by
`test_dead_node_stops_owning_its_gids()` in
`tests/test_tcp_gid_resolve.c`, the eviction and liveness rows in
`tests/test_tcp_thread_stop.c`, and the churn rounds in
`tests/py/test_mesh_stress.py`.

## Connection lifetime

- A manager reaps connections that never registered
  (`tcp_reap_pending_conns()`) and retires a registered node's connection
  when it hangs up or stops answering (`tcp_health_check_retire()`). It
  never connects to a node: a worker owns its link to the manager and
  reconnects itself (`tcp_worker_manager_pass()`).
- A worker closes a dead connection to another worker on its
  peer-connect thread's next pass (`tcp_worker_peer_sweep()`), and they
  reconnect. The accept thread and `tcp_worker_publish_peer()` replace a
  dead connection, never a live one.
- `TcpConnection` is reference counted, so the data path may hold one
  after it leaves its table; see its comment for the rules.
- A failed `accept()` (e.g. `EMFILE`, which leaves the connection queued)
  is retried after 100 ms. Without that backoff it once logged 295 GB.
- Registration hits a wall at about 40 nodes (measured on 64). The
  full-mesh connection cost behind it is inferred, **not** confirmed in
  the code.

## No Ethernet or DHCP path

Guest TCP/IP uses the host TAP/bridge
([`ionic_eth_net.c`](../ionic/ionic_eth_net.c)); the mesh carries only
`TCP_MSG_IONIC` and control traffic. Don't rebuild guest networking
inside it. The removed Ethernet and DHCP relay paths' ordinals remain as
`TCP_MSG_RESERVED_18..20`: renumbering the message types would change
the wire format, which means bumping `TCP_PROTOCOL_VERSION` (checked at
registration and at the worker-to-worker handshake) and cutting off
every unupgraded peer.
