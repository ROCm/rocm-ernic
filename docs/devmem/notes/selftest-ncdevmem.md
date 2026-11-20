# ncdevmem / devmem.py as the userspace driver for ionic devmem TX

Scope: `tools/testing/selftests/drivers/net/hw/{ncdevmem.c,devmem.py,devmem_lib.py,Makefile}`,
`tools/testing/selftests/net/ynl.mk`, `tools/net/ynl/`, and the kernel paths they hit.

Citation prefixes:

- `M:` = `/var/tmp/ionic-devmem/linux` at **v7.4-rc3**
- `S:` = `/var/tmp/ionic-devmem/linux-7.2.15` at **v7.2.15**
- `NC` = `tools/testing/selftests/drivers/net/hw/ncdevmem.c`
- `LIB` = `tools/testing/selftests/drivers/net/hw/devmem_lib.py`

Every claim is marked VERIFIED (read in source, or run on this host) or INFERRED.
Nothing in any kernel tree was modified; the build experiment ran in a `git archive` export under
`/tmp/ncdevmem-build.lunkHf` (`git status --short` in `M` is empty afterwards).

## 0. Decisive facts

1. VERIFIED. **`devmem.py` cannot be used as-is on ionic.** Every test, including `check_tx`, first
   calls `require_devmem()`, which runs `ncdevmem -f <ifname>` with no `-s`/`-c`. That mode is
   `run_devmem_tests()`: RX-only checks that need `ethtool -X`, TCP header split, and `bind-rx`.
   ionic has none of them, so the probe exits non-zero and all tests SKIP with "Test requires devmem
   support" (`M:LIB:45-68`, `S:LIB:11-19`; `M:NC:1494-1516`, `M:NC:1089-1183`).
2. VERIFIED. **`ncdevmem` in client (TX) mode runs no RX setup**: no header split, RSS, flow
   steering, or `bind-rx`. The only non-TX thing it does is an ethtool `channels-get` to count RX
   queues, and it refuses to run with fewer than 2 unless `-t` and `-q` are both given
   (`M:NC:1518-1534`, `S:NC:1488-1504`). So call it directly: `ncdevmem -f IF -s PEER -p PORT -t 0 -q 1`.
3. VERIFIED. ncdevmem TX always sends from **dmabuf offset 0**, one `sendmsg()` per input *line*,
   **copies each line into the udmabuf first**, and blocks up to 500 ms for one zerocopy completion
   per `sendmsg` (`M:NC:1350-1410`). It is a functional smoke test, not a zero-copy benchmark, and
   it cannot express a non-zero start offset, close/unbind with sends in flight, or a wrong-netdev
   route. A small extra program is needed (section 7).
4. VERIFIED. The kernel caps every devmem frag at one page (`PAGE_SIZE - off`) and never coalesces
   adjacent net_iovs, and returns `-EMSGSIZE` at `MAX_SKB_FRAGS` so TCP starts a new skb
   (`M:net/core/datagram.c:706-744`, `M:net/core/devmem.c:453-464`, `M:net/ipv4/tcp.c:1353-1358`;
   `S:net/core/datagram.c:707-739`, `S:net/core/devmem.c:430-439`). `-z N` therefore directly sets
   the frag size the driver sees: `-z 3` gives skbs of up to MAX_SKB_FRAGS 3-byte frags.
5. VERIFIED (built and run here). ncdevmem from the v7.4-rc3 tree builds on this Ubuntu 24.04.4 /
   gcc 13.3.0 / glibc 2.39 host in ~3.5 s, also fully static (`LDFLAGS=-static`, 1.1 MB, no
   warnings). A partial tree export must contain `MAINTAINERS` or `ynl_gen_c.py` spins forever.
6. VERIFIED. There is **no netlink dump, procfs, debugfs, or tracepoint for TX bindings**; the
   `dmabuf` attribute exists only on `page-pool-get` and `queue-get` (RX). Proof that skbs were
   unreadable has to come from a kprobe/fentry on the driver (section 8).
7. VERIFIED. A devmem send can never legitimately report `SO_EE_CODE_ZEROCOPY_COPIED`
   (`skb_copy_ubufs()` returns `-EFAULT` for unreadable skbs), and ncdevmem does not look at
   `ee_code` at all (`M:net/core/skbuff.c:2003-2011`, `M:NC:1253-1267`).

## 1. Command-line options

`getopt` string: `"Lls:c:p:v:q:t:f:z:nb:"` at v7.4-rc3 (`M:NC:1431`), `"Lls:c:p:v:q:t:f:z:n"` at
v7.2.15 (`S:NC:1414`). `-b` (rx-page-size, RX only) is the only option added. VERIFIED.

| Opt | Variable | Meaning | TX relevance | Cite (M) |
| --- | --- | --- | --- | --- |
| `-l` | `is_server` | listen = devmem **RX** server. Absent = client = devmem **TX** | omit for TX | NC:1436-1438, 1565 |
| `-s ADDR` | `server_ip` | server address (client connects to it). IPv6, or IPv4 turned into `::ffff:a.b.c.d` | required | NC:1439, 1549-1552, 809-831 |
| `-c ADDR` | `client_ip` | client source address; client `bind()`s to it **with the same port as `-p`** | optional | NC:1442, 1298-1304, 1327-1333 |
| `-p PORT` | `port` | TCP port (server port, and client source port if `-c`) | required | NC:1445, 1554-1557 |
| `-f IFNAME` | `ifname` | netdev; `ifindex = if_nametoindex()`. Used for `SO_BINDTODEVICE` and `bind-tx` | required | NC:1457, 1485-1490 |
| `-z N` | `max_chunk` | split each line into iovecs of N bytes (last one shorter); max `MAX_IOV` = 1024 iovecs | TX only | NC:90, 1460, 1358-1378 |
| `-t N` | `start_queue` | first RX queue to bind | pass `-t 0` to skip the queue-count check | NC:1454, 1518-1547 |
| `-q N` | `num_queues` | number of RX queues to bind | pass `-q 1` with `-t` | NC:1451 |
| `-n` | `skip_config` | skip ethtool config; **only read in `do_server()`** | no effect in TX (devmem.py passes it for netkit) | NC:1463, 872, 1075-1085 |
| `-v N` | `do_validation` | RX-side pattern check, period N | RX server only | NC:1448, 278-305, 1033-1036 |
| `-L` | `fail_on_linear` | RX: fail on `SCM_DEVMEM_LINEAR` | RX only | NC:1433, 999-1002 |
| `-b N` | `rx_page_size` | RX niov size; >PAGE_SIZE makes the udmabuf hugetlb-backed | RX; **not in v7.2.15** | NC:1466-1478, 164-166 |

There is **no option** for dmabuf size, dmabuf offset, ifindex, completion timeout, or iteration
count. VERIFIED:

- dmabuf size is fixed: `getpagesize() * NUM_PAGES`, `NUM_PAGES` = 16000, i.e. 65,536,000 bytes
  (62.5 MiB) (`M:NC:83,1559`; `S:NC:81,1529`). It is allocated in every mode, client included.
- completion wait is fixed: `waittime_ms = 500` (`M:NC:104`, `S:NC:101`).
- No argument (no `-s`, no `-c`) selects the RX self-test `run_devmem_tests()` (`M:NC:1494-1516`).

## 2. TX path, step by step (client mode)

All VERIFIED in `M:NC`; v7.2.15 line numbers in brackets. The TX code is byte-identical between
the two tags (the `git diff v7.2.15 v7.4-rc3` of ncdevmem.c touches only includes, `-b`, hugetlb
memfd, and `open("/dev/udmabuf", O_RDWR -> O_RDONLY)`).

1. `main()`: `ifindex = if_nametoindex(ifname)` (1490). If neither `-t` nor `-q` given:
   `rxq_num(ifindex)` = ethtool netlink `channels-get`, `rx_count + combined_count`; fail with
   "number of device queues is too low" if < 2 (1518-1534) [1488-1504]. `rxq_num()` calls
   `ethtool_channels_get_rsp_free(rsp)` with `rsp == NULL` when the GET fails (386-411), and the
   generated free dereferences it (`ethtool-user.c:4649-4653` in the build tree): **VERIFIED by
   running it on `lo`: SIGSEGV**. ionic implements `get_channels`
   (`M:.../pensando/ionic/ionic_ethtool.c:759-775`, combined_count = `lif->nxqs`),
   so the GET works, but a 1-queue LIF fails the `< 2` check. `-t 0 -q 1` skips both.
2. `provider->alloc()` -> `udmabuf_alloc()` (145-221) [141-]:
   `open("/dev/udmabuf")`, `memfd_create("udmabuf-test", MFD_ALLOW_SEALING)`,
   `fcntl(F_ADD_SEALS, F_SEAL_SHRINK)`, `ftruncate(size)`, `ioctl(UDMABUF_CREATE, {memfd, offset 0,
   size})` (197) [182], then `mmap()` the **dmabuf fd** read/write shared.
3. `do_client()` (1275-1423) [1258-]:
   - `socket(AF_INET6, SOCK_STREAM)` (1306), `SO_REUSEPORT` + `SO_REUSEADDR` (789-807).
   - `setsockopt(SO_BINDTODEVICE, ifname)` (1315-1320) [1298]. Unconditional.
   - `bind_tx_queue(ifindex, mem->fd, &ys)` (1322, 746-787) [729-]: new `ynl_sock` on the `netdev`
     family, `netdev_bind_tx()` with attributes `ifindex` and `fd` only; reply `id` stored in
     `tx_dmabuf_id`. **The ynl socket is kept open until exit; closing it is the unbind**
     (`ynl_sock_destroy(ys)` at 1419 -> `close()` in `M:tools/net/ynl/lib/ynl.c:806-810` ->
     `netdev_nl_sock_priv_destroy()` -> `net_devmem_unbind_dmabuf()`,
     `M:net/core/netdev-genl.c:1470-1495`, `S:net/core/netdev-genl.c:1425-1450`).
   - optional `bind(client_ip:port)` (1327-1333).
   - `setsockopt(SO_ZEROCOPY, 1)` (1335) [1318].
   - `connect()` (1344).
4. Send loop, one iteration per `getline(stdin)` (1350-1410) [1333-1393]:

```c
		if (max_chunk) {
			msg.msg_iovlen =
				(line_size + max_chunk - 1) / max_chunk;
			...
			for (int i = 0; i < msg.msg_iovlen; i++) {
				iov[i].iov_base = (void *)(i * max_chunk);
				iov[i].iov_len = max_chunk;
			}
			iov[msg.msg_iovlen - 1].iov_len =
				line_size - (msg.msg_iovlen - 1) * max_chunk;
		} else {
			iov[0].iov_base = 0;
			iov[0].iov_len = line_size;
			msg.msg_iovlen = 1;
		}

		msg.msg_iov = iov;
		provider->memcpy_to_device(mem, 0, line, line_size);
		...
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_DEVMEM_DMABUF;
		cmsg->cmsg_len = CMSG_LEN(sizeof(__u32));
		*((__u32 *)CMSG_DATA(cmsg)) = ddmabuf;

		ret = sendmsg(socket_fd, &msg, MSG_ZEROCOPY);
		...
		if (ret != line_size) { pr_err("Did not send all bytes ..."); goto err_free_line; }
		if (wait_compl(socket_fd))
			goto err_free_line;
```

   - `iov_base` is **an offset into the dmabuf**, not a pointer; always starts at 0.
   - `memcpy_to_device()` brackets a plain `memcpy()` into the mmap with `DMA_BUF_IOCTL_SYNC`
     START/END (232-244). The line (including its `\n`) is copied to offset 0 with **no bound
     check against the 62.5 MiB buffer**.
   - blocking socket, so `sendmsg` returns the whole line or an error.
5. `wait_compl()` (1210-1273) [1193-1254]: until `now + 500 ms`, `poll()` then
   `recvmsg(MSG_ERRQUEUE)`; accepts cmsg `SOL_IP/IP_RECVERR` or `SOL_IPV6/IPV6_RECVERR`, requires
   `ee_origin == SO_EE_ORIGIN_ZEROCOPY` and `ee_errno == 0`, prints `tx complete [lo,hi]`
   (`lo = ee_info`, `hi = ee_data`) and **returns after the first notification**. No notification in
   500 ms -> "did not receive tx completion", exit 1. `ee_code` is never read. Note `do_poll()` leaves
   `pfd.events` uninitialised (1193-1208); it only works because `POLLERR` is always reported.
6. EOF on stdin: prints `ncdevmem: tx ok`, `ynl_sock_destroy()` (unbind), `close(socket)`,
   `udmabuf_free()`. Order is always unbind -> close socket -> free dmabuf (1412-1422, 1571-1572).

Kernel side of each step (VERIFIED, M with S in brackets):

- `bind-tx`: `netdev_nl_bind_tx_doit()` `M:net/core/netdev-genl.c:1194-1288` [`S:...:1155-`]:
  `-ENODEV` if no such dev or not present; `-EOPNOTSUPP` "Driver does not support netmem TX" when
  `netdev->netmem_tx == NETMEM_TX_NONE` (1241-1246) [1201-1206]; dma device from
  `netdev_queue_get_dma_dev(dev, 0, TX)` = `dev->dev.parent` with a `dma_mask`
  (`M:net/core/netdev_queues.c:9-21`, same lines in S), else `-EOPNOTSUPP` "Device doesn't support
  DMA" (`M:net/core/devmem.c:203-206`, `S:net/core/devmem.c:199`). `netmem_tx` is the 3-value enum
  `NETMEM_TX_NONE/DMA/NO_DMA` at both tags (`M:include/linux/netdevice.h:1822-1826,2180`,
  `S:include/linux/netdevice.h:1811,2166`): **ionic must set `NETMEM_TX_DMA`, not `true`**.
- The op has no `GENL_ADMIN_PERM` at either tag (`M:net/core/netdev-genl-gen.c:240-244`,
  `S:net/core/netdev-genl-gen.c:233-237`). `SO_BINDTODEVICE` still needs CAP_NET_RAW, so ncdevmem
  needs root regardless (INFERRED from general socket semantics, not re-read here).
- dmabuf constraints: size and every DMA sg entry must be PAGE_SIZE aligned
  (`M:net/core/devmem.c:268-274,302-313`; `S:net/core/devmem.c:245-248,276-279`). v7.2.15 builds
  the binding on a genpool, v7.4-rc3 on a flat niov array: same uAPI, different internals.
- `SCM_DEVMEM_DMABUF` cmsg must be exactly `CMSG_LEN(sizeof(u32))`
  (`M:net/core/sock.c:3194-3198`, `S:net/core/sock.c:3123`).
- `tcp_sendmsg_locked()` (`M:net/ipv4/tcp.c:1122-1260`): needs `MSG_ZEROCOPY` flag **and**
  `SOCK_ZEROCOPY`, else the cmsg yields `-EINVAL` (1178-1181) [S:1172-1175]; `binding =
  net_devmem_get_binding(sk, id)` (1164-1171) [S:1158-1159]; devmem and non-devmem data never share
  an skb (`skb_frags_readable(skb) != !binding` forces a new segment, 1256-1257) [S:1243].
- `net_devmem_get_binding()` (`M:net/core/devmem.c:399-451`, `S:net/core/devmem.c:377-427`):
  `-EINVAL` for unknown id / RX binding; `-ENODEV` when the socket's dst device is neither
  `binding->dev` nor `binding->vdev`. v7.4-rc3 additionally requires the binding's netns and uid to
  match the socket's (408-413); **v7.2.15 has no netns/uid check** (S:385-389).

## 3. How devmem.py validates TX

VERIFIED (`M:LIB:172-269`, `S:LIB:105-203`; `M:devmem.py:17-26`):

- Receiver is **socat** on the remote endpoint: `socat -U - TCP{4|6}-LISTEN:{port},reuseaddr`
  (`M:LIB:214-216`; v7.2.15 has no `,reuseaddr`, `S:LIB:147-149`). ncat is not used.
- `run_tx`: `bash -c 'echo -e "hello\nworld" | ncdevmem -f IF -s REMOTE -p PORT'`; pass iff
  `socat.stdout.strip() == "hello\nworld"` (`M:LIB:242-254`). That is two `sendmsg` calls of 6 bytes.
- `run_tx_chunks`: same with `-z 3` (`M:LIB:257-269`): 2 iovecs of 3 bytes per line.
- There is **no pattern check, no large transfer, and no check of ncdevmem's exit status** in the TX
  tests (`cmd()` raises on non-zero by default, so a failed completion wait does fail the test;
  INFERRED from the lib default, not re-read).
- The `-v` pattern validator exists only in the ncdevmem **RX server** (`validate_buffer()`,
  `M:NC:278-305`): bytes 1..N-1 then `\n`, as produced by
  `yes $(echo -e \\x01\\x02\\x03\\x04\\x05\\x06) | head -c ...` with `-v 7`. It needs devmem RX,
  which ionic does not have.
- Total TX coverage upstream: 12 bytes, 1-2 frags per skb, never more than one page.

For ionic use an ordinary receiver and a hash: `socat -u TCP6-LISTEN:PORT,reuseaddr - | sha256sum`
on the peer versus `sha256sum` of the input file (INFERRED recipe).

## 4. Limits and things that fail on ionic

VERIFIED unless marked.

| Item | Finding | Cite |
| --- | --- | --- |
| IPv6 only? | **No.** Socket is `AF_INET6` but `parse_address()` falls back to IPv4 and builds a v4-mapped address; devmem.py picks `TCP4`/`TCP6` by `cfg.addr_ipver`. Needs `net.ipv6.bindv6only=0` and IPv6 compiled in (INFERRED). Only the netkit variant does `cfg.require_ipver('6')` | M:NC:809-831; M:LIB:92,197,216 |
| `SO_BINDTODEVICE` | ncdevmem always sets it. The **kernel does not require it**: `net_devmem_get_binding()` only compares the dst device. So ncdevmem cannot test "route via another netdev" | M:NC:1315-1320; M:net/core/devmem.c:430-439 |
| Queue config in TX mode | only `channels-get`; needs >= 2 RX queues unless `-t`/`-q` given; NULL-deref crash if the GET fails | M:NC:1518-1534, 386-411 |
| RX-only steps in TX mode | none in `do_client()`. Header split, RSS, ntuple, bind-rx are in `do_server()` and `run_devmem_tests()` only | M:NC:872-900, 1103-1167 |
| devmem.py probe | runs `run_devmem_tests()`: `ethtool -X IF equal N`, `rings-set tcp-data-split enabled` (+`hds-thresh 0`), `bind-rx`, `channels-set`. ionic: `supported_ring_params` is only TX/RX push, no `queue_mgmt_ops`, no ntuple -> probe fails -> **every test SKIPs** | M:NC:1089-1183; M:LIB:45-68; M:.../pensando/ionic/ionic_ethtool.c:1150 |
| devmem.py env | needs `NetDrvEpEnv` (local + remote endpoint over ssh/netns via `net.config`), deploys the binary to the remote, needs socat on the remote | M:devmem.py:41-42; M:LIB:219-222 |
| `-c` | binds the client to the **server port number** too | M:NC:1298-1304 |
| line model | one `sendmsg` per `\n`-terminated line; binary data containing `\n` is split; a line > 62.5 MiB overflows the mmap; with `-z`, line <= 1024 * N bytes | M:NC:1353-1381 |
| completion wait | 500 ms per `sendmsg`, fixed; a slow emulator/peer ACK fails the run | M:NC:104,1212 |
| udmabuf size | v7.2.15 `size_limit_mb` defaults to **64** (62.5 MiB just fits); v7.4-rc3 defaults to INT_MAX | S:drivers/dma-buf/udmabuf.c:23; M:drivers/dma-buf/udmabuf.c:23 |
| SG / csum prerequisites | `zc = MSG_ZEROCOPY` only if `sk_route_caps & NETIF_F_SG`. For TCP with GSO allowed, `sk_setup_caps()` ORs in `NETIF_F_SG | NETIF_F_HW_CSUM` regardless of the device, so the send is accepted and the skb is then dropped in `validate_xmit_skb()` (`__skb_linearize` -> `__pskb_pull_tail` returns NULL, or `skb_checksum_help` returns `-EFAULT`), counted in `tx_dropped` | M:net/ipv4/tcp.c:1158-1162; M:net/core/sock.c:2679-2691; M:net/core/dev.c:3679-3681,4245-4263; M:net/core/skbuff.c:2906-2907 |

The last row matches finding F1/W1 in `ernic-emu-datapath.md`: the emulator must grant TX_SG and
TX_CSUM or the test hangs in retransmission with rising `tx_dropped`, not with an error.

## 5. Building ncdevmem standalone

### 5.1 What the build needs (VERIFIED)

- `YNL_GENS := ethtool netdev`, `YNL_GEN_FILES := ncdevmem toeplitz ...`
  (`M:tools/testing/selftests/.../net/hw/Makefile:79-93`, `S:...Makefile:70-83`).
- `ynl.mk` builds `libynl.a` by `make -C tools/net/ynl GENS="ethtool netdev" RSTS="" libynl.a` and
  adds `-I usr/include $(KHDR_INCLUDES) -I tools/net/ynl/lib -I tools/net/ynl/generated`
  (`M:tools/testing/selftests/net/ynl.mk:16-33`).
- `libynl.a` = `lib/ynl.o` + `generated/*-user.o` (`M:tools/net/ynl/Makefile:24-26`). The generated
  `netdev-user.[ch]` / `ethtool-user.[ch]` come from `pyynl/ynl_gen_c.py` reading
  `Documentation/netlink/specs/{netdev,ethtool}.yaml` (`M:tools/net/ynl/generated/Makefile:44-50`),
  compiled against `include/uapi` of the tree (`-idirafter $(UAPI_PATH)`, `Makefile.deps:10,23-26,32`).
- ncdevmem.c itself needs tree uapi headers newer than Ubuntu 24.04's: `linux/netdev.h`
  (bind-tx), `linux/ethtool_netlink.h`, `linux/udmabuf.h`, `linux/errqueue.h`,
  `SCM_DEVMEM_DMABUF` from `asm/socket.h` -> so `make headers` first (`usr/include`).
- Host tools: gcc, make, python3 with `yaml` (PyYAML; `jsonschema` optional). No libmnl, no liburing
  (only `iou-zcrx` wants it; a warning is printed), no clang (only the `*.bpf.c` objects want it and
  they are not built when you name the ncdevmem target).

### 5.2 Exact commands (VERIFIED by running them here, v7.4-rc3)

In a full tree:

```bash
make headers                                   # installs usr/include
D=$PWD/tools/testing/selftests/drivers/net/hw
make -C "$D" "$D/ncdevmem"                     # dynamic
make -C "$D" "$D/ncdevmem" LDFLAGS=-static     # static
```

The target must be the **absolute** `$(OUTPUT)/ncdevmem` path. `make -C $D ncdevmem` falls through
to make's built-in rule and fails with `netdev-user.h: No such file or directory` (observed).

Without touching the kernel checkout (what was done for this note):

```bash
S=$(mktemp -d)
git -C /var/tmp/ionic-devmem/linux archive v7.4-rc3 MAINTAINERS Makefile Kbuild Kconfig scripts \
    include/uapi include/linux/compiler_types.h arch/x86 tools/net/ynl tools/scripts tools/build \
    tools/include tools/arch/x86 tools/testing/selftests Documentation/netlink usr | tar -x -C "$S"
make -C "$S" headers
D=$S/tools/testing/selftests/drivers/net/hw
make -C "$D" "$D/ncdevmem" LDFLAGS=-static
```

- **`MAINTAINERS` is mandatory**: `find_kernel_root()` walks up looking for it in a `while True`
  with no termination at `/` (`M:tools/net/ynl/pyynl/ynl_gen_c.py:3475-3482`,
  `S:...:3429-3436`). Without it the generator burned 10 CPU-minutes before being killed (observed).
- Results: dynamic 266,720 bytes, needs only `libc.so.6`, highest symbol version `GLIBC_2.38`;
  static 1,096,552 bytes, `statically linked`, no link warnings (no `getaddrinfo`/NSS use; it uses
  `inet_pton`, `if_nametoindex`, `popen`).
- The static binary ran on this host: `-f lo -s ::1 -p 5201 -t 0 -q 1` created the udmabuf and got
  `netdev_bind_tx: Operation not supported`, i.e. the netlink path works end to end.

### 5.3 Does a v7.4-rc3-built binary run in a v7.2.15 guest?

- VERIFIED: the TX code path is identical at both tags and the `bind-tx` request carries only
  `ifindex` and `fd`, both in the v7.2.15 policy (`S:net/core/netdev-genl-gen.c:124-128`). The only
  new attribute (`rx-page-size`) is sent only with `-b` (`M:NC:717-718`).
- VERIFIED: static linking removes any guest glibc dependency. The dynamic binary needs guest
  glibc >= 2.38.
- INFERRED: therefore the v7.4-rc3 static binary works unchanged on v7.2.15 for TX, provided `-b`
  is not used. Not executed in a guest. `popen("ethtool ...")` is only reached in RX paths.
- Alternative with zero doubt: build from the v7.2.15 tree with the same commands (the Makefile
  pieces are the same; not built here).

## 6. Covering the test matrix

What plain ncdevmem can do (INFERRED recipes built on the VERIFIED behaviour above; peer is
`socat -u TCP-LISTEN:PORT,reuseaddr - | sha256sum`, base command
`NC="ncdevmem -f IF -s PEER -p PORT -t 0 -q 1"`):

| Case | With ncdevmem | Gap |
| --- | --- | --- |
| Non-page-aligned **lengths** | yes: any line length. `head -c 5000 /dev/zero | tr '\0' a | $NC` -> frags 4096 + 904 | - |
| Non-page-aligned **offsets** | partly: `-z N` with N not dividing 4096 makes every iovec after the first start at `i*N`, e.g. `-z 4097`, `-z 1000`, `-z 3` | first frag is always offset 0; no way to start mid-page or at a high page |
| Many frags per skb | yes: `-z 3` .. `-z 64` on a multi-KB line gives MAX_SKB_FRAGS tiny frags per skb, up to 1024 iovecs per `sendmsg` | line <= 1024 * N bytes |
| Page-crossing iovec | yes: `-z 6000` -> each iovec splits into 2 frags at the page boundary | - |
| MTU 1500 / jumbo | yes: `ip link set IF mtu 9000` (plus peer/TAP/bridge), rerun | - |
| TSO on/off | yes: `ethtool -K IF tso off` / `on`; with TSO off and SG on, software GSO splits by frag reference | emulator has no TSO today (ernic-emu-datapath F5) |
| tx checksum off | yes, as a **negative** test: `ethtool -K IF tx off` -> sends are accepted, packets dropped in `validate_xmit_skb()`, ncdevmem dies on "did not receive tx completion"; assert `tx_dropped` rises and nothing reaches `ionic_start_xmit` | no positive variant exists by design |
| Route via a different netdev | no: `SO_BINDTODEVICE` pins it | need extra program |
| close / unbind / rmmod with sends in flight | no: strictly sendmsg -> wait completion -> next; unbind only at exit | need extra program. Crude variant: `kill -9` ncdevmem mid-stream, or `rmmod ionic` while it runs |
| Sustained send | weak: `yes "$(head -c 1048575 /dev/zero | tr '\0' x)" | head -c 10G | $NC` sends 1 MiB lines, but each is memcpy'd first and waits for a completion | throughput number is not meaningful |

Why the frag shapes are predictable (VERIFIED):

```c
	while (length && iov_iter_count(from)) {
		if (i == MAX_SKB_FRAGS)
			return -EMSGSIZE;

		virt_addr = (size_t)iter_iov_addr(from);
		niov = net_devmem_get_niov_at(binding, virt_addr, &off, &size);
		if (!niov)
			return -EFAULT;

		size = min_t(size_t, size, length);
		size = min_t(size_t, size, iter_iov_len(from));

		netmem_inc_ref(net_iov_to_netmem(niov));
		skb_add_rx_frag_netmem(skb, i, net_iov_to_netmem(niov), off,
				       size, PAGE_SIZE);
```

(`M:net/core/datagram.c:726-741`; `S:net/core/datagram.c:724-741`), with
`*off = virt_addr % PAGE_SIZE; *size = PAGE_SIZE - *off;` (`M:net/core/devmem.c:460-461`,
`S:net/core/devmem.c:436-437`). An offset >= dmabuf size gives `-EFAULT` from `sendmsg`.

## 7. The small extra program needed (`devmem_tx.c`, proposal)

INFERRED design; every kernel behaviour it relies on is cited above. Reuse ncdevmem's
`udmabuf_alloc()`, `bind_tx_queue()` and `wait_compl()` verbatim; link the same `libynl.a`.

Options:

- `-f IF`, `-s ADDR`, `-p PORT` as ncdevmem; `-B` = do **not** `SO_BINDTODEVICE`.
- `-S bytes` dmabuf size (page multiple); `-o off` first-byte offset in the dmabuf; `-l len` bytes
  per `sendmsg`; `-z chunk`; `-g gap` bytes skipped between iovecs (non-adjacent pages);
  `-n count` or `-T seconds`.
- `-P seed` fill the whole dmabuf once with a position-dependent pattern (e.g. 32-bit counter of
  the dmabuf offset xor seed) so the receiver can verify without a second copy; the companion
  receiver mode (`-r`) recomputes it from the same `-o/-l/-z/-g` and compares.
- `-w N` pipeline depth: allow N un-notified sends before draining the error queue (sustained
  send); read all notifications, track `[lo,hi]` ranges, and **assert
  `!(ee_code & SO_EE_CODE_ZEROCOPY_COPIED)`**.
- `-m copy|zc|devmem`: same loop with plain `send()`, `MSG_ZEROCOPY` from anonymous memory, or
  devmem. Gives the three-way baseline from one binary with identical framing.
- `-X stage` teardown-under-load: after queueing `-w N` sends without waiting, do one of
  `close-sock` (close TCP socket first), `unbind` (close the ynl socket first; later sends with the
  stale id must return `-EINVAL`), `free-dmabuf` (munmap + close dmabuf fd first), `exit`
  (`_exit(0)` with everything in flight), or `pause` (print pid and wait, so the harness can
  `rmmod ionic`, `ip link set IF down`, or change MTU/queue count underneath).
- `-R` after connect, wait for the harness to re-route (`ip route replace PEER dev OTHER`) and
  expect `sendmsg` to fail with `-ENODEV` (`M:net/core/devmem.c:433-439`); already-queued skbs that
  get re-routed are dropped by `validate_xmit_unreadable_skb()` (`M:net/core/dev.c:4169-4195`,
  `S:net/core/dev.c:4048-`).

Things the teardown cases probe (VERIFIED facts, INFERRED risk):

- In-flight skbs pin the binding: each net_iov frag holds `binding->ref`; the dmabuf is unmapped
  from a workqueue only when the last ref drops (`M:net/core/devmem.c:58-90`, comment at
  `M:net/core/devmem.h:36-52`). So after `unbind` the driver may still transmit and must still be
  able to complete and free those skbs; DMA addresses stay valid until then.
- **Module unload while a TX binding exists**: `binding->dev` is cleared only in
  `mp_dmabuf_devmem_uninstall()`, which runs per bound **RX** queue
  (`M:net/core/devmem.c:545-563`, `M:net/core/dev.c:12642-12649`). A TX binding has no bound RX
  queue, so nothing clears `binding->dev` on unregister, and the later
  `netdev_nl_sock_priv_destroy()` does `netdev_hold(binding->dev)` / `netdev_lock(dev)`
  (`M:net/core/netdev-genl.c:1478-1492`, `S:net/core/netdev-genl.c:1432-1447`). INFERRED: `rmmod
  ionic` (or hot-unplug) with ncdevmem still holding its netlink socket can touch a freed
  net_device. This is core code, not ionic, but the `rmmod` test will hit it under KASAN; I did not
  find a NETDEV_UNREGISTER hook that handles TX bindings (grep of `net/core/dev.c`,
  `net/core/devmem.c`, `net/core/netdev-genl.c` at both tags). OPEN: confirm or find the mitigation.

## 8. Observability: proving the skbs were unreadable devmem skbs

VERIFIED what exists / does not exist:

- **Netlink**: `dmabuf` appears only in the `page-pool` and `queue` attribute sets and the `dmabuf`
  bind set (`M:Documentation/netlink/specs/netdev.yaml:201-202,345-346,586-616`). There is no
  `bind-tx` dump/get. `cli.py --family netdev --dump page-pool-get` / `queue-get` show RX bindings
  only. The one netlink signal for TX is the `bind-tx` reply id (ncdevmem prints
  `got tx dmabuf id=N`) and the extack strings on failure.
- **Tracepoints**: none for devmem/netmem (`grep devmem|dmabuf include/trace/events/*.h` hits only
  `dma_buf.h`). Useful neighbours:
  - `dma_buf:dma_buf_dynamic_attach` / `dma_buf_detach` / `dma_buf_get` / `dma_buf_put`
    (`M:include/trace/events/dma_buf.h:117-149`): shows bind attaching the ionic PCI device and the
    deferred detach after the last skb is freed. Check presence at v7.2.15 before relying on it
    (not checked).
  - `dma:dma_map_sg` / `dma_unmap_sg` (`M:include/trace/events/dma.h:284,376`): one map at bind,
    one unmap at release, and **no `dma_map_page`/`dma_map_single` for payload frags** while
    sending. That absence is the driver-level proof that it did not DMA-map devmem frags itself.
  - `net:net_dev_start_xmit` has `len`, `data_len`, `gso_size`, `gso_segs`, `ip_summed` but no
    nr_frags and no unreadable flag (`M:include/trace/events/net.h:15-39`).
  - `tcp:tcp_sendmsg_locked` (`M:net/ipv4/tcp.c:1254`), `skb:kfree_skb` (drop reason for the
    validate_xmit drops), `skb:consume_skb`.
- **`skb->unreadable`**: bitfield `__u8 unreadable:1` (`M:include/linux/skbuff.h:1039`), set by
  `skb_add_rx_frag_netmem()` when the netmem is a net_iov (`M:include/linux/skbuff.h:2609-2610`),
  read through `skb_frags_readable()` (1880-1882).
- **procfs/debugfs**: nothing devmem-specific. `/sys/kernel/debug/dma_buf/bufinfo` (DEBUG_FS) lists
  the udmabuf and its attachments (attached device = ionic PCI dev) (INFERRED from general dma-buf
  debugfs; not re-read).
- **Completion code**: `SO_EE_CODE_ZEROCOPY_COPIED` is set when `uarg->zerocopy` was cleared
  (`M:net/core/skbuff.c:1855-1864`, `S:net/core/skbuff.c:1868`), which happens on
  `skb_copy_ubufs()` success or when the route lacks SG (`M:net/ipv4/tcp.c:1158-1162`). For devmem
  `skb_copy_ubufs()` returns `-EFAULT` (`M:net/core/skbuff.c:2010-2011`), so a devmem completion
  with COPIED set would be a kernel bug; `ee_code == 0` is necessary but **not sufficient** proof
  (it says nobody copied, not that the NIC got net_iovs). Also `msg_zerocopy_realloc(..., devmem)`
  skips pinned-page accounting (`M:net/core/skbuff.c:1725,1740,1793`), so `ulimit -l` does not bite.

Recommended proof (INFERRED recipes; function names VERIFIED at
`M:.../pensando/ionic/ionic_txrx.c:1103,1120,1435,1650,1820`):

```bash
# 1. Every skb entering the driver from the test flow is unreadable, with frag count histogram
bpftrace -e '
kprobe:ionic_start_xmit {
  $skb = (struct sk_buff *)arg0;
  $sh  = (struct skb_shared_info *)($skb->head + $skb->end);
  @unreadable[$skb->unreadable] = count();
  if ($skb->unreadable) { @frags = lhist($sh->nr_frags, 0, 18, 1); @len = hist($skb->len); }
}'
# 2. The driver never dma-maps payload while the test runs (expect only header maps)
bpftrace -e 'tracepoint:dma:dma_map_page { @[kstack(5)] = count(); }'
# 3. Binding lifetime: attach at bind, detach only after last skb freed
trace-cmd record -e dma_buf:dma_buf_dynamic_attach -e dma_buf:dma_buf_detach -e dma:dma_map_sg -e dma:dma_unmap_sg
# 4. Drops on the negative tests
bpftrace -e 'tracepoint:skb:kfree_skb { @[args->reason, kstack(6)] = count(); }'
```

Bitfield access (`$skb->unreadable`) needs BTF (`CONFIG_DEBUG_INFO_BTF`) or kernel headers; with
the DKMS module, kprobes on `ionic_*` work as long as the functions are not inlined (fall back to
`kprobe:dev_hard_start_xmit` filtered on `$dev->name`). After the patch, add a driver counter
(ethtool `-S` `tx_devmem_frags` or a `netdev_dbg`) as the cheapest permanent proof.

Emulator-side proof is impossible by construction: udmabuf pages are ordinary guest RAM, so the
bytes look the same whichever path the driver took (`ernic-emu-datapath.md` V4).

## 9. Guest kernel CONFIG

VERIFIED from Kconfig:

- `NET_DEVMEM`: `def_bool y`, `depends on DMA_SHARED_BUFFER` and `PAGE_POOL` at v7.4-rc3
  (`M:net/Kconfig:69-72`); at v7.2.15 it also `select GENERIC_ALLOCATOR`
  (`S:net/Kconfig:69-72`). Not user-selectable: it turns on when its dependencies are on.
- `PAGE_POOL`: no prompt; selected by `IONIC` (`M:drivers/net/ethernet/pensando/Kconfig:26`).
- `DMA_SHARED_BUFFER`: no prompt (`M:drivers/dma-buf/Kconfig`); selected by DRM, `SYNC_FILE`
  (line 7), heaps. A defconfig without DRM may lack it, which silently removes `NET_DEVMEM`.
  `CONFIG_SYNC_FILE=y` (in the selftest config) or `DMABUF_HEAPS` forces it.
- `UDMABUF`: `bool`, default n, depends on `DMA_SHARED_BUFFER`, `MEMFD_CREATE`, `MMU`
  (`M:drivers/dma-buf/Kconfig:33-38`). Built-in only, so it cannot be added later as a module.
- `MEMFD_CREATE`: prompt only under `EXPERT`, otherwise on (`M:mm/Kconfig:1332-1333`).
- Selftest fragment: `CONFIG_NET_DEVMEM=y`, `CONFIG_UDMABUF=y`, `CONFIG_SYNC_FILE=y`,
  `CONFIG_HUGETLBFS=y`, `CONFIG_IPV6=y` among others
  (`M:tools/testing/selftests/.../net/hw/config`).
- `DEBUG_NET` (`M:net/Kconfig.debug:21-23`, needs `DEBUG_KERNEL`): enables
  `DEBUG_NET_WARN_ON_ONCE`, including the one in `net_iov_inc_ref()/dec_ref()` for non-devmem
  net_iovs (`M:net/core/skbuff.c:7707-7731`).

Recommended set (INFERRED; the bundle config has none of UDMABUF/KASAN/lockdep per
`ernic-vm-stack.md` B6):

```text
# functional
CONFIG_NET_DEVMEM=y  CONFIG_PAGE_POOL=y  CONFIG_DMA_SHARED_BUFFER=y  CONFIG_SYNC_FILE=y
CONFIG_UDMABUF=y     CONFIG_MEMFD_CREATE=y  CONFIG_SHMEM=y  CONFIG_TMPFS=y
CONFIG_IONIC=m       CONFIG_IPV6=y
# debug
CONFIG_DEBUG_KERNEL=y  CONFIG_DEBUG_NET=y
CONFIG_KASAN=y  CONFIG_KASAN_GENERIC=y  CONFIG_KASAN_INLINE=y
CONFIG_PROVE_LOCKING=y  CONFIG_DEBUG_ATOMIC_SLEEP=y  CONFIG_DEBUG_LOCKDEP=y
CONFIG_DMA_API_DEBUG=y  CONFIG_DMA_API_DEBUG_SG=y
CONFIG_PAGE_POOL_STATS=y  CONFIG_DEBUG_FS=y  CONFIG_DMABUF_DEBUG=y
# observability
CONFIG_FTRACE=y  CONFIG_FUNCTION_TRACER=y  CONFIG_KPROBES=y  CONFIG_KPROBE_EVENTS=y
CONFIG_BPF_SYSCALL=y  CONFIG_BPF_JIT=y  CONFIG_BPF_EVENTS=y
CONFIG_DEBUG_INFO_DWARF5=y  CONFIG_DEBUG_INFO_BTF=y  CONFIG_DEBUG_INFO_BTF_MODULES=y
```

`DMA_API_DEBUG` is the most valuable one for this patch: it flags any `dma_unmap_*` of an address
the driver did not map, which is exactly the bug of unmapping a net_iov frag on TX completion.
The DKMS module must be rebuilt against the debug kernel (KASAN and lockdep change struct layout).

## 10. Baselines for performance comparison

- VERIFIED: `tools/testing/selftests/net/msg_zerocopy.c` is the upstream copy-vs-MSG_ZEROCOPY
  tool. getopt `"46c:C:D:i:l:mp:rs:S:t:vzZ:"` (`M:tools/testing/selftests/net/msg_zerocopy.c:721`):
  `-4/-6`, `-D daddr`, `-S saddr`, `-p port`, `-s payload_len` (max ~64 KiB, buffer is
  `IP_MAXPACKET`, :97), `-t seconds`, `-C cpu`, `-r` receiver, `-z` use MSG_ZEROCOPY, `-Z 0|1`
  assert the completion's copied/zerocopy state (:447-454), `-l` notification batch, last arg the
  test (`tcp`). It needs no ynl: `make -C tools/testing/selftests/net msg_zerocopy` style build, or
  plain `gcc -O2 msg_zerocopy.c` against `usr/include` (not built here).

```bash
# peer
./msg_zerocopy -6 -D PEER -S GUEST -p 5202 -r tcp
# copy baseline
./msg_zerocopy -6 -D PEER -S GUEST -p 5202 -t 10 tcp
# MSG_ZEROCOPY baseline, asserting it was not silently copied
./msg_zerocopy -6 -D PEER -S GUEST -p 5202 -t 10 -z -Z 1 tcp
```

- ncdevmem is not comparable to these: it does getline + memcpy per send and one completion
  round-trip per send (section 2). Use `devmem_tx -m copy|zc|devmem` (section 7) with identical
  `-l`, `-w` for the three-way comparison, against the same `-r` sink.
- INFERRED: with the emulator (no TSO, software checksum fill, TAP), the device dominates; expect
  the comparison to show correctness and CPU-per-byte in the guest, not wire throughput.

## 11. Open questions

1. `rmmod ionic` with a live TX binding: is the stale `binding->dev` in
   `netdev_nl_sock_priv_destroy()` a real use-after-free at v7.4-rc3 / v7.2.15 (section 7)? Needs a
   KASAN run or a closer read of the unregister path.
2. Not executed in a guest: the v7.4-rc3 static binary on v7.2.15 (expected fine, section 5.3).
3. `dma_buf:*` tracepoints: present at v7.4-rc3; presence at v7.2.15 not checked.
4. How many queues does the emulated LIF expose? If 1, ncdevmem needs `-t 0 -q 1` (harmless to
   always pass).
5. The emulator's ACK latency versus ncdevmem's fixed 500 ms completion wait under KASAN + lockdep.
6. bpftrace/BTF availability in the guest image, and whether `ionic_start_xmit` in the DKMS build
   is kprobe-able by name.
7. iperf3's zerocopy mode was not evaluated as a baseline (it is not in the tree).

## Verification pass

Independent re-read of the source at both tags (`M:` = v7.4-rc3, `S:` = v7.2.15, same prefixes as
above), plus a re-run of the static binary left in `/tmp/ncdevmem-build.lunkHf`. Nothing above this
heading was edited. Verdicts: CONFIRMED, WRONG (with correction), UNVERIFIABLE.

### Decisive facts (section 0)

| # | Claim | Verdict | Evidence / correction |
| --- | --- | --- | --- |
| 0.1 | `devmem.py` probes with `ncdevmem -f IF` before every test, TX included, so all tests SKIP on ionic | CONFIRMED | `M:LIB:45-68,242-245,257-260`; `S:LIB:11-19,175-178,190-193`. No-address mode is `run_devmem_tests()` (`M:NC:1494-1515`). On ionic the first failing step is `configure_headersplit()` (`M:NC:1115`), not `ethtool -X`: ionic has `set_rxfh` (`M:ionic_ethtool.c:1172`) but `supported_ring_params` is TX/RX push only (`:1150`). Outcome unchanged. At v7.4-rc3 the probe also writes `/proc/sys/vm/nr_hugepages` and probes `-b 16384` (`M:LIB:60-65`) |
| 0.2 | Client mode does no RX setup; only `channels-get`, skipped by `-t 0 -q 1` | CONFIRMED | `M:NC:1518-1547`, `do_client()` `M:NC:1275-1423`. Both `-t` and `-q` are required: `-t 0` alone exits "Missing -q argument" (re-run here) |
| 0.2b | Failed `channels-get` segfaults | CONFIRMED | re-run here: `ncdevmem -f lo -s ::1 -p 5201` exits 139; `rxq_num()` frees a NULL rsp (`M:NC:402-406`) |
| 0.3 | Offset 0 only, one `sendmsg` per line, memcpy first, one completion per send | CONFIRMED | `M:NC:1350-1410`. Detail: `do_poll()` blocks up to 500 ms inside a loop bounded by 500 ms, so the worst-case wait is about 1 s, not 500 ms (`M:NC:1201,1212,1223`) |
| 0.4 | One page max per frag, no coalescing, `-EMSGSIZE` at `MAX_SKB_FRAGS` | CONFIRMED | `M:net/core/datagram.c:726-744`, `S:net/core/datagram.c:724-741`, `M:net/core/devmem.c:453-464`, `S:net/core/devmem.c:429-440`, `M:net/ipv4/tcp.c:1353-1358` |
| 0.4b | So `-z 3` etc. is a usable positive test of many-frag skbs | WRONG for ionic as it stands | See correction C1 below |
| 0.5 | Builds on this host, static 1.1 MB | CONFIRMED | binary present: 1,096,552 bytes, `statically linked`; `find_kernel_root()` loop `M:tools/net/ynl/pyynl/ynl_gen_c.py:3475-3482` |
| 0.6 | No netlink dump, procfs, debugfs or tracepoint for TX bindings | CONFIRMED | `dmabuf` only in page-pool/queue sets and the bind set (`M:Documentation/netlink/specs/netdev.yaml:201,345,586-616`); ops are `bind-rx` (:824) and `bind-tx` (:852), both `do` only. `include/trace/events/` has devmem/netmem hits only in `page_pool.h` |
| 0.7 | devmem never reports `ZEROCOPY_COPIED`; ncdevmem ignores `ee_code` | CONFIRMED | `M:net/core/skbuff.c:1862-1864,2010-2011`; `M:NC:1253-1267` |

### Other load-bearing claims

| Claim | Verdict | Evidence / correction |
| --- | --- | --- |
| TX path of ncdevmem.c identical at both tags | CONFIRMED | full `diff` of the two files: only includes, `-b`, hugetlb memfd, `O_RDWR -> O_RDONLY`, `rx_page_size` in bind-rx |
| `netmem_tx` is a 3-value enum at both tags; ionic must set `NETMEM_TX_DMA` | CONFIRMED | `M:include/linux/netdevice.h:1822-1826,2180`; `S:include/linux/netdevice.h:1811-1814,2166`; consumers `M:net/core/netdev-genl.c:1169,1241`, `S:net/core/netdev-genl.c:1135,1201`; `M:net/core/dev.c:4176-4179`, `S:net/core/dev.c:4055-4058`. No ionic file sets it at either tag |
| `bind-tx` unprivileged at both tags | CONFIRMED | `M:net/core/netdev-genl-gen.c:240-244`, `S:net/core/netdev-genl-gen.c:233-237`; v7.4-rc3 documents it as intentional (`M:net/core/netdev-genl.c:1189-1194`) |
| "`SO_BINDTODEVICE` still needs CAP_NET_RAW, so ncdevmem needs root regardless" (section 2, INFERRED) | WRONG | The capability is required only to *re*-bind an already bound socket: `if (sk->sk_bound_dev_if && !ns_capable(...CAP_NET_RAW))` (`M:net/core/sock.c:644`). The first bind is unprivileged. What actually needs privilege is opening `/dev/udmabuf` (a misc device; `root:kvm 0660` on this host), which is a udev/permissions matter |
| bind-tx request has only `ifindex` + `fd`, valid for the v7.2.15 policy; a v7.4-rc3 binary works on v7.2.15 | CONFIRMED by reading, UNVERIFIABLE by execution | `S:net/core/netdev-genl-gen.c:124-128`; the uapi enum diff between the tags only appends `NETDEV_A_DMABUF_RX_PAGE_SIZE`, so attribute numbers match. Not run in a guest |
| v7.2.15 has no netns/uid check on the binding; v7.4-rc3 has | CONFIRMED | `S:net/core/devmem.c:385-389`, `M:net/core/devmem.c:407-413`. Harmless for ncdevmem (same process owns both sockets) |
| dmabuf alignment constraints | CONFIRMED with a version difference | v7.4-rc3 checks size, sg address and sg length for both directions (`M:net/core/devmem.c:268-274,313-320`); v7.2.15 checks dmabuf size for TX only and sg length only (`S:net/core/devmem.c:244-248,276-280`). v7.4-rc3 also attaches with `dma_buf_dynamic_attach()` + `dma_buf_pin()` (`M:net/core/devmem.c:237-251`); v7.2.15 uses plain `dma_buf_attach()` (`S:net/core/devmem.c:229`) |
| udmabuf `size_limit_mb` 64 at v7.2.15, INT_MAX at v7.4-rc3 | CONFIRMED | `S:drivers/dma-buf/udmabuf.c:23`, `M:drivers/dma-buf/udmabuf.c:23`; 16000 pages <= 16384-page limit |
| Kernel does not require `SO_BINDTODEVICE`; wrong route gives `-ENODEV` | CONFIRMED | `M:net/core/devmem.c:433-439`, `S:net/core/devmem.c:409-415` |
| "tx checksum off / SG off: skbs dropped in `validate_xmit_skb`, ncdevmem times out" | PARTLY WRONG | See correction C2 |
| `dma_buf:*` tracepoints (open question 3) | CONFIRMED present at v7.2.15 | `S:include/trace/events/dma_buf.h:124,132`, fired at `S:drivers/dma-buf/dma-buf.c:1062,1109` (plain `dma_buf_attach()` goes through the same function). Open question 3 is closed |
| Kconfig: `NET_DEVMEM` `def_bool y` behind `DMA_SHARED_BUFFER` + `PAGE_POOL`; `UDMABUF` bool default n; `SYNC_FILE` selects `DMA_SHARED_BUFFER` | CONFIRMED | `M:net/Kconfig:69-72`, `S:net/Kconfig:69-73`, `M:drivers/dma-buf/Kconfig:4-7,33-38`, `M:mm/Kconfig:1332-1333`. Note `PAGE_POOL` comes from `select` in `IONIC` (`pensando/Kconfig:26`); a guest kernel built with `IONIC=n` and the driver supplied only by DKMS needs another selector of `PAGE_POOL` or `NET_DEVMEM` silently disappears |
| `msg_zerocopy.c` baselines | CONFIRMED | getopt `M:tools/testing/selftests/net/msg_zerocopy.c:721`; `-Z` check `:447-454` |
| `rmmod ionic` with a live TX binding may be a use-after-free in core (section 7) | UNVERIFIABLE by reading; could not refute | `binding->dev` is written only at bind (`M:net/core/devmem.c:219`) and cleared only in `mp_dmabuf_devmem_uninstall()` under `xa_empty(&bound_rxqs)` (`M:net/core/devmem.c:545-563`), which a TX binding never reaches. The binding takes no netdev reference (no `netdev_hold` in `net/core/devmem.c`). The netdev notifier only sends DEL_NTF (`M:net/core/netdev-genl.c:1508-1518`, `S:...:1463-1467`). `netdev_nl_sock_priv_destroy()` then does `netdev_hold(dev)` / `netdev_lock(dev)` on the stale pointer (`M:...:1480-1492`, `S:...:1435-1447`). No fix found in `git log v7.2.15..v7.4-rc3 -- net/core/devmem.c net/core/netdev-genl.c`. Same code at both tags. Needs a KASAN run; it is core code, not something the ionic patch can fix |

### Corrections that change the test plan or the patch

**C1. ionic cannot transmit a devmem skb with more frags than `max_sg_elems`, so small `-z` values
are an expected failure, not a positive test.** `ionic_tx_descs_needed()` falls back to
`skb_linearize()` when a non-GSO skb has `nr_frags > q->max_sg_elems`, or when one TSO segment needs
more than `max_sg_elems + 1` buffers (`M:.../pensando/ionic/ionic_txrx.c:1720-1723,
1754-1757,1772-1778`). `max_sg_elems` is forced to `min(IONIC_TX_MAX_FRAGS - 1, MAX_SKB_FRAGS)` = 15
(`M:ionic_lif.c:3982-3989`, `M:ionic_dev.h:213`, `M:ionic_if.h:1023`), while core fills devmem skbs
up to `MAX_SKB_FRAGS` (>= 17, `M:net/Kconfig:292-295`). `skb_linearize()` fails on an unreadable skb
(`__pskb_pull_tail` returns NULL, `M:net/core/skbuff.c:2906-2907`), the driver drops, TCP
retransmits the same skb, and the connection stalls. With ncdevmem this is reached by any `-z N`
where an skb fills to 17 frags: roughly `N <= MSS/17` (non-GSO skb with 17 frags), and for larger
`N` whenever a segment needs more than 16 buffers, i.e. about `N < MSS/15` (about 100 bytes at MTU
1500, about 600 at MTU 9000). Upstream's `-z 3` test passes only because it sends 6-byte lines
(2 frags). Consequences:

- The matrix rows "Many frags per skb: yes, `-z 3` .. `-z 64`" and the summary bullet "`-z 3` gives
  skbs full of 3-byte frags" describe what core builds, not what ionic can send. On an unpatched
  frag limit they hang with `tx_dropped`/driver drop counters rising and `tx_linearize` unchanged.
- This is the one place where the selftest evidence bears on "is devmem TX on ionic a driver-only
  change": core offers no per-netdev cap on devmem frags per skb, and the driver can neither
  linearize nor split a non-GSO skb, so a driver-only patch must choose to drop such skbs (a stall
  the sender inflicts on its own socket). `ionic-tx-device-contract.md` (C2) and
  `kernel-core-devmem-tx.md` reach the same conclusion from the driver side.
- Test plan: treat `-z N` below the thresholds above as a negative test (assert a clean drop, no
  crash, no leak), and use `-z` >= ~128 at MTU 1500 (or lines shorter than 16 chunks) for the
  positive many-frag case.

**C2. "Checksum/SG off means dropped packets and a ncdevmem timeout" is only true for some feature
combinations.** At both tags turning tx checksum off does not remove `NETIF_F_SG` any more
(`netdev_fix_features()` has no SG-needs-csum rule, `M:net/core/dev.c:11181-11222`). Traced cases
for a device with `NETMEM_TX_DMA`:

- csum off, SG on: sub-MSS skb -> `skb_checksum_help()` `-EFAULT` (`M:net/core/dev.c:3679-3681`);
  GSO skb -> `skb_segment()` `sg=1, csum=0` -> `__skb_linearize(nskb)` fails
  (`M:net/core/skbuff.c:5183-5185`). Dropped. Claim holds.
- SG off, csum on: sub-MSS -> `skb_needs_linearize` fails (`M:net/core/dev.c:4246-4248`); GSO ->
  `skb_copy_bits()` `-EFAULT` -> `goto err` (`M:net/core/skbuff.c:5080-5081`). Dropped. Claim holds.
- **SG off and csum off, send larger than one MSS**: `skb_segment()` `sg=0, csum=0` calls
  `skb_copy_and_csum_bits()` (`M:net/core/skbuff.c:5068-5078`, `S:net/core/skbuff.c:4982-4992`),
  which on an unreadable skb zero-fills the destination and returns 0, with only a
  `DEBUG_NET_WARN_ON_ONCE` at v7.4-rc3 (`M:net/core/skbuff.c:3724-3729`;
  `S:net/core/skbuff.c:3636-3640`). The segments go out with zero
  payload and a matching checksum, are ACKed, and ncdevmem prints `tx ok`. This is silent data
  corruption, not a drop or a timeout. It is the case an emulator granting neither TX_SG nor
  TX_CSUM produces, so the receiver-side hash (section 3) is mandatory, not optional, and the
  `devmem_tx` pattern check must run in every feature combination. (`kernel-core-devmem-tx.md`
  section 3 table has the same trace, and reports uninitialised bytes instead of zeros at v7.2.6.)

**C3. Minor.** v7.2.15 lacks the v7.4-rc3 rejection of devmem TX combined with TCP fastopen/repair
(`M:net/ipv4/tcp.c:1186-1189`, absent at `S:net/ipv4/tcp.c:1177-1181`); irrelevant to ncdevmem,
which uses neither.
