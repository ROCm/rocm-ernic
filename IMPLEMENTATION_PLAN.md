# PVRDMA Server Implementation Plan

## Overview
Based on analysis of the Linux kernel driver and QEMU implementation,
here's the roadmap for completing the PVRDMA device server.

## Device Initialization Sequence

### 1. Driver → Device: DSR Setup
```
Driver writes:  PVRDMA_REG_DSRLOW  (low 32 bits of DSR physical address)
Driver writes:  PVRDMA_REG_DSRHIGH (high 32 bits of DSR physical address)

Device response:
- Map DSR structure from guest memory
- Map command/response slots
- Map async event ring pages  
- Map CQ notification ring pages
- Fill in device capabilities in DSR
```

### 2. Driver → Device: Activation
```
Driver writes:  PVRDMA_REG_CTL = PVRDMA_DEVICE_CTL_ACTIVATE

Device response:
- Initialize RDMA backend (libibverbs)
- Activate device state
```

### 3. Driver → Device: Command Processing
```
Driver writes:  PVRDMA_REG_REQUEST = 0

Device response:
- Read command from cmd_slot
- Execute command (CREATE_QP, CREATE_CQ, REG_MR, etc.)
- Write response to resp_slot
- Trigger interrupt (vector 0 - INTR_VEC_CMD_RING)
```

### 4. Driver ← Device: UAR Doorbells
```
Driver writes to BAR2 (UAR):
- QP Send/Recv doorbells
- CQ Arm/Poll operations  
- SRQ Recv operations

Device response:
- Process work requests
- Post completions to CQ
- Trigger CQ interrupts (vector 2 - INTR_VEC_CMD_COMPLETION_Q)
```

## Key Data Structures

### Device Shared Region (DSR)
```c
struct pvrdma_device_shared_region {
    u64 cmd_slot_dma;           // Command slot DMA address
    u64 resp_slot_dma;          // Response slot DMA address
    struct pvrdma_ring_page_info async_ring_pages;  // Async events
    struct pvrdma_ring_page_info cq_ring_pages;     // CQ notifications
    struct pvrdma_device_caps caps;                  // Device capabilities
};
```

### Register Map (BAR1)
```
PVRDMA_REG_DSRLOW  = 0x00  // DSR low address
PVRDMA_REG_DSRHIGH = 0x04  // DSR high address
PVRDMA_REG_CTL     = 0x08  // Control register (activate/reset/unquiesce)
PVRDMA_REG_REQUEST = 0x0C  // Command request trigger
PVRDMA_REG_ERR     = 0x10  // Error code
PVRDMA_REG_ICR     = 0x14  // Interrupt cause register
PVRDMA_REG_IMR     = 0x18  // Interrupt mask register
PVRDMA_REG_MACL    = 0x1C  // MAC address low
PVRDMA_REG_MACH    = 0x20  // MAC address high
```

## Implementation Phases

### Phase 1: DSR and Register Handling ✓ (NEXT)
**Files to integrate:**
- `src/from-qemu/hw/rdma/vmw/pvrdma_main.c`
  - `pvrdma_regs_read()` / `pvrdma_regs_write()`
  - `load_dsr()` - Map DSR from guest memory
  - `init_dsr_dev_caps()` - Fill device capabilities
  - `init_dev_ring()` - Initialize ring buffers

**Bridge to libvfio-user:**
- Map BAR1 register writes to device functions
- Use `vfu_addr_to_sgl()` / `vfu_sgl_get()` for DMA mapping
- Track DSR state in device structure

### Phase 2: RDMA Backend Integration
**Files to integrate:**
- `src/from-qemu/hw/rdma/rdma_backend.c`
  - `rdma_backend_init()` - Initialize libibverbs
  - `rdma_backend_create_pd()`, `create_qp()`, `create_cq()`, etc.
  
- `src/from-qemu/hw/rdma/rdma_rm.c`
  - `rdma_rm_init()` - Resource manager
  - Resource allocation/deallocation

**Bridge to libvfio-user:**
- Wrap QEMU memory functions with vfio-user DMA operations
- Replace QEMU types (PCIDevice → vfu_ctx_t)

### Phase 3: Command Channel
**Files to integrate:**
- `src/from-qemu/hw/rdma/vmw/pvrdma_cmd.c`
  - `pvrdma_exec_cmd()` - Command dispatcher
  - Individual command handlers (create_cq, create_qp, reg_mr, etc.)

**Bridge to libvfio-user:**
- Trigger MSI-X interrupts after command completion
- Use `vfu_irq_trigger()` for interrupt delivery

### Phase 4: UAR and Doorbells
**Files to integrate:**
- `src/from-qemu/hw/rdma/vmw/pvrdma_main.c`
  - `pvrdma_uar_write()` - Doorbell handler
  
- `src/from-qemu/hw/rdma/vmw/pvrdma_qp_ops.c`
  - `pvrdma_post_send()`, `pvrdma_post_recv()`
  - `pvrdma_post_cqe()` - Post completion

**Bridge to libvfio-user:**
- Process BAR2 writes as doorbell operations
- Trigger CQ interrupts on completions

### Phase 5: Ring Buffers
**Files to integrate:**
- `src/from-qemu/hw/rdma/vmw/pvrdma_dev_ring.c`
  - Ring buffer operations for async events and CQ notifications

## Dependencies to Resolve

### QEMU → libvfio-user Mappings

| QEMU Concept | libvfio-user Equivalent |
|--------------|-------------------------|
| `PCIDevice *pci_dev` | `vfu_ctx_t *vfu_ctx` |
| `MemoryRegion` | BAR access callbacks |
| `rdma_pci_dma_map()` | `vfu_addr_to_sgl()` + `vfu_sgl_get()` |
| `rdma_pci_dma_unmap()` | `vfu_sgl_put()` |
| `msix_notify()` | `vfu_irq_trigger()` |
| `dma_addr_t` (guest PA) | `vfu_dma_addr_t` |
| QEMU Error handling | Standard errno |

### Files Already in Place
- ✓ `src/from-qemu/hw/rdma/rdma_utils.c` - Utility functions
- ✓ `src/from-qemu/hw/rdma/rdma_rm_defs.h` - Resource manager defs
- ✓ `src/from-qemu/hw/rdma/rdma_backend_defs.h` - Backend defs
- ✓ `src/from-qemu/include/qemu-extra/` - QEMU stub headers

## Testing Strategy

### Unit Tests
1. DSR mapping and initialization
2. Register read/write operations
3. Command processing (without real IB hardware)
4. Doorbell handling

### Integration Tests
1. Driver loads and initializes device
2. Create PD, MR, CQ, QP
3. Post send/recv operations
4. Verify completions

### Hardware Tests (with real InfiniBand)
1. Connect to mlx5 or rxe device
2. Run rdma-core tests
3. Performance benchmarking

## Next Steps (Priority Order)

1. **Create compatibility bridge layer** (`src/vfu_compat_bridge.c`)
   - Wrap QEMU types and functions for libvfio-user environment
   
2. **Implement register handling** (BAR1 read/write with DSR logic)
   - Hook up `pvrdma_regs_read()` / `pvrdma_regs_write()` to BAR1 callback
   
3. **Implement DSR initialization**
   - Map guest memory for DSR structure
   - Initialize rings and capabilities
   
4. **Integrate RDMA backend**
   - Initialize libibverbs
   - Connect to host InfiniBand device
   
5. **Implement command channel**
   - Process CREATE_QP, CREATE_CQ, REG_MR, etc.
   - Trigger command completion interrupts

## Estimated Effort

- Phase 1 (DSR/Registers): 1-2 days
- Phase 2 (RDMA Backend): 2-3 days  
- Phase 3 (Command Channel): 2-3 days
- Phase 4 (UAR/Doorbells): 1-2 days
- Phase 5 (Ring Buffers): 1 day
- Testing & Debug: 2-3 days

**Total: ~2 weeks for full implementation**

