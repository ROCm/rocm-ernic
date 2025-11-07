# Multi-Backend Architecture Design

**Goal**: Support multiple RDMA backends (none, loopback, verbs, future backends)

---

## Current Architecture (Single Backend)

```
PVRDMADev
  └─> RdmaBackendDev (struct ibv_* types)
       └─> libibverbs functions directly
```

**Problem**: Tightly coupled to libibverbs, can't have alternative backends.

---

## New Architecture (Multi-Backend)

```
PVRDMADev
  └─> RdmaBackendDev
       ├─> backend_type (enum)
       ├─> backend_ops (vtable)
       └─> backend_private (void*)
            │
            ├─> RdmaBackendNone (no hardware)
            ├─> RdmaBackendLoopback (internal emulation) ← NEW!
            ├─> RdmaBackendVerbs (libibverbs)
            └─> RdmaBackendVirtio (future?)
```

---

## Backend Interface (Operations Vtable)

```c
typedef struct RdmaBackendOps {
    /* Backend lifecycle */
    int (*init)(RdmaBackendDev *dev, const char *config);
    void (*fini)(RdmaBackendDev *dev);
    
    /* Query operations */
    int (*query_port)(RdmaBackendDev *dev, struct ibv_port_attr *attr);
    int (*query_device)(RdmaBackendDev *dev, struct ibv_device_attr *attr);
    
    /* Protection Domain */
    int (*create_pd)(RdmaBackendDev *dev, RdmaBackendPD *pd);
    void (*destroy_pd)(RdmaBackendPD *pd);
    
    /* Memory Region */
    int (*create_mr)(RdmaBackendMR *mr, RdmaBackendPD *pd, 
                     void *addr, size_t length, 
                     uint64_t guest_start, int access);
    void (*destroy_mr)(RdmaBackendMR *mr);
    uint32_t (*mr_lkey)(const RdmaBackendMR *mr);
    uint32_t (*mr_rkey)(const RdmaBackendMR *mr);
    
    /* Completion Queue */
    int (*create_cq)(RdmaBackendDev *dev, RdmaBackendCQ *cq, int cqe);
    void (*destroy_cq)(RdmaBackendCQ *cq);
    void (*poll_cq)(RdmaDeviceResources *rdma_dev_res, RdmaBackendCQ *cq);
    
    /* Queue Pair */
    int (*create_qp)(RdmaBackendQP *qp, uint8_t qp_type,
                     RdmaBackendPD *pd, RdmaBackendCQ *scq, RdmaBackendCQ *rcq,
                     RdmaBackendSRQ *srq,
                     uint32_t max_send_wr, uint32_t max_recv_wr,
                     uint32_t max_send_sge, uint32_t max_recv_sge);
    void (*destroy_qp)(RdmaBackendQP *qp, RdmaDeviceResources *dev_res);
    uint32_t (*qpn)(const RdmaBackendQP *qp);
    
    /* QP State Transitions */
    int (*qp_state_init)(RdmaBackendDev *dev, RdmaBackendQP *qp,
                         uint8_t qp_type, uint32_t qkey);
    int (*qp_state_rtr)(RdmaBackendDev *dev, RdmaBackendQP *qp,
                        uint8_t qp_type, uint8_t sgid_idx,
                        union ibv_gid *dgid, uint32_t dqpn,
                        uint32_t rq_psn, uint32_t qkey, bool qkey_set);
    int (*qp_state_rts)(RdmaBackendQP *qp, uint8_t qp_type,
                        uint32_t sq_psn, uint32_t qkey, bool qkey_set);
    int (*query_qp)(RdmaBackendQP *qp, struct ibv_qp_attr *attr,
                    int attr_mask, struct ibv_qp_init_attr *init_attr);
    
    /* Data Path */
    void (*post_send)(RdmaBackendDev *dev, RdmaBackendQP *qp,
                      uint8_t qp_type, struct ibv_sge *sge, uint32_t num_sge,
                      uint8_t sgid_idx, union ibv_gid *sgid, union ibv_gid *dgid,
                      uint32_t dqpn, uint32_t dqkey, void *ctx);
    void (*post_recv)(RdmaBackendDev *dev, RdmaBackendQP *qp,
                      uint8_t qp_type, struct ibv_sge *sge, uint32_t num_sge,
                      void *ctx);
    
    /* GID management */
    int (*add_gid)(RdmaBackendDev *dev, const char *ifname, union ibv_gid *gid);
    int (*del_gid)(RdmaBackendDev *dev, const char *ifname, int gid_idx);
    
    /* SRQ operations (optional) */
    int (*create_srq)(RdmaBackendSRQ *srq, RdmaBackendPD *pd,
                      uint32_t max_wr, uint32_t max_sge, uint32_t srq_limit);
    void (*destroy_srq)(RdmaBackendSRQ *srq);
    
    const char *name;  /* Backend name for identification */
} RdmaBackendOps;
```

---

## Backend Types

```c
typedef enum {
    RDMA_BACKEND_TYPE_NONE,      /* No backend (current no-backend mode) */
    RDMA_BACKEND_TYPE_LOOPBACK,  /* Internal loopback emulation */
    RDMA_BACKEND_TYPE_VERBS,     /* libibverbs hardware */
    RDMA_BACKEND_TYPE_VIRTIO,    /* Future: virtio-rdma */
    RDMA_BACKEND_TYPE_MAX
} RdmaBackendType;
```

---

## Loopback Backend Design

### Purpose
Emulate a complete RDMA connection internally without hardware or external process.

### Components

```c
typedef struct RdmaLoopbackPD {
    uint32_t handle;
} RdmaLoopbackPD;

typedef struct RdmaLoopbackMR {
    uint32_t handle;
    void *virt;
    size_t length;
    uint64_t guest_start;
    int access_flags;
    uint32_t lkey;
    uint32_t rkey;
} RdmaLoopbackMR;

typedef struct RdmaLoopbackCQ {
    uint32_t handle;
    int cqe;
    GQueue *completion_queue;  /* List of work completions */
    QemuMutex lock;
} RdmaLoopbackCQ;

typedef struct RdmaLoopbackQP {
    uint32_t qpn;
    uint8_t qp_type;
    enum ibv_qp_state state;
    uint32_t qkey;
    
    /* Send/Recv queues */
    GQueue *send_queue;
    GQueue *recv_queue;
    
    /* Associated CQs */
    RdmaLoopbackCQ *scq;
    RdmaLoopbackCQ *rcq;
    
    /* Connection info (for RC QPs) */
    uint32_t remote_qpn;
    union ibv_gid remote_gid;
    
    QemuMutex lock;
} RdmaLoopbackQP;

typedef struct RdmaBackendLoopback {
    /* Resource tracking */
    GHashTable *pds;    /* handle -> RdmaLoopbackPD */
    GHashTable *mrs;    /* handle -> RdmaLoopbackMR */
    GHashTable *cqs;    /* handle -> RdmaLoopbackCQ */
    GHashTable *qps;    /* qpn -> RdmaLoopbackQP */
    
    uint32_t next_qpn;
    
    /* For loopback connections */
    GHashTable *qp_pairs;  /* local_qpn -> remote_qpn mapping */
    
    QemuMutex lock;
} RdmaBackendLoopback;
```

### Loopback Behavior

#### Queue Pairs
- **UD (Unreliable Datagram)**: Messages delivered to same-QP recv queue
- **RC (Reliable Connection)**: After `modify_qp(RTR)`, can create pair with another QP
- **Self-loopback**: QP can send to itself (useful for testing)

#### Memory Operations
- `create_mr`: Track virtual address and guest address mapping
- `post_send/recv`: Copy data between MRs internally
- No actual DMA needed - direct memory copy

#### Completions
- `post_send`: Generate send completion immediately
- `post_recv`: Complete when matching send arrives
- `poll_cq`: Return completions from internal queue

---

## Migration Strategy

### Phase 1: Create Backend Abstraction ✅ (Step 1)
1. Define `RdmaBackendOps` structure
2. Add `backend_type` and `backend_ops` to `RdmaBackendDev`
3. Add backend selection at init time

### Phase 2: Wrap Existing "None" Backend ✅ (Step 2)
1. Create `rdma_backend_none.c`
2. Implement ops that return immediately (current behavior)
3. Register as default when no hardware

### Phase 3: Wrap Verbs Backend ✅ (Step 3)
1. Keep existing `rdma_backend.c` functions
2. Create `rdma_backend_verbs.c` wrapper
3. Ops call existing functions

### Phase 4: Implement Loopback Backend 🎯 (Step 4)
1. Create `rdma_backend_loopback.c`
2. Implement internal emulation
3. Test with driver

### Phase 5: Update Call Sites ✅ (Step 5)
1. Change direct backend calls to use ops vtable
2. Update `rdma_rm.c` to use `backend_dev->ops->xxx()`
3. Maintain compatibility

---

## Configuration

### Command Line
```bash
# No backend (default, what we have now)
./vfu_pvrdma -s /tmp/socket.sock

# Loopback backend
./vfu_pvrdma -s /tmp/socket.sock --backend loopback

# Verbs backend (real hardware)
./vfu_pvrdma -s /tmp/socket.sock --backend verbs:mlx5_0

# Future: virtio backend
./vfu_pvrdma -s /tmp/socket.sock --backend virtio:/dev/vhost-rdma
```

### Backend Selection
```c
RdmaBackendType rdma_backend_get_type_from_string(const char *backend_str) {
    if (!backend_str || !strcmp(backend_str, "none")) {
        return RDMA_BACKEND_TYPE_NONE;
    } else if (!strcmp(backend_str, "loopback")) {
        return RDMA_BACKEND_TYPE_LOOPBACK;
    } else if (!strncmp(backend_str, "verbs:", 6)) {
        return RDMA_BACKEND_TYPE_VERBS;
    }
    return RDMA_BACKEND_TYPE_NONE;
}
```

---

## Benefits

### For Development
- ✅ Test RDMA operations without hardware
- ✅ Reproducible behavior (loopback is deterministic)
- ✅ Fast iteration (no real network)
- ✅ Works in CI/CD

### For Testing
- ✅ Unit test individual operations
- ✅ Test connection establishment
- ✅ Test data transfer
- ✅ Test error conditions

### For Production
- ✅ Fallback when hardware unavailable
- ✅ Multiple backend support
- ✅ Easy to add new backends

---

## Testing Strategy

### Unit Tests
```c
// Test loopback PD allocation
test_loopback_pd() {
    backend = create_loopback_backend();
    pd = backend->ops->create_pd(...);
    assert(pd != NULL);
    backend->ops->destroy_pd(pd);
}

// Test loopback data transfer
test_loopback_send_recv() {
    // Create QPs
    qp1 = create_qp(...);
    qp2 = create_qp(...);
    
    // Connect them
    modify_qp(qp1, RTR, remote_qpn=qp2);
    modify_qp(qp2, RTR, remote_qpn=qp1);
    
    // Post recv on qp2
    post_recv(qp2, buffer);
    
    // Send from qp1
    post_send(qp1, data);
    
    // Poll completion
    poll_cq(qp2->rcq);
    
    // Verify data
    assert(buffer == data);
}
```

### Integration Tests
1. Load driver with loopback backend
2. Create IB resources
3. Perform RDMA operations
4. Verify completions

---

## Implementation Priority

1. **HIGH**: Backend abstraction layer
2. **HIGH**: None backend wrapper (current behavior)
3. **HIGH**: Loopback backend basic ops (PD, MR, CQ, QP)
4. **MEDIUM**: Loopback data path (send/recv)
5. **MEDIUM**: Verbs backend wrapper
6. **LOW**: Advanced features (SRQ, etc.)

---

## File Structure

```
src/
├── from-qemu/hw/rdma/
│   ├── rdma_backend.h              (updated with ops)
│   ├── rdma_backend_defs.h         (updated with types)
│   ├── rdma_backend_core.c         (NEW - abstraction layer)
│   ├── rdma_backend_none.c         (NEW - none backend)
│   ├── rdma_backend_loopback.c     (NEW - loopback backend)
│   ├── rdma_backend_verbs.c        (wrapper for existing code)
│   └── rdma_backend.c              (keep existing verbs functions)
```

---

## Next Steps

1. Implement backend abstraction layer
2. Create "none" backend
3. Create loopback backend skeleton
4. Implement loopback PD/MR/CQ/QP
5. Test with driver

Ready to proceed? 🚀

