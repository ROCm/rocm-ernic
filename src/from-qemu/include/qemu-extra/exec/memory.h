/*
 * Stub for QEMU Memory API
 * PVRDMA uses MemoryRegion for BARs, but libvfio-user handles this
 */

#ifndef QEMU_MEMORY_H
#define QEMU_MEMORY_H

#include <stdint.h>
#include <stdbool.h>

typedef uint64_t hwaddr;
typedef uint64_t Int128;  /* Simplified - QEMU uses int128_t */

/* Minimal MemoryRegion stub */
typedef struct MemoryRegion {
    /* Stubbed - libvfio-user manages our BARs */
    const char *name;
    hwaddr addr;
    hwaddr size;
} MemoryRegion;

/* AddressSpace - stubbed, we use libvfio-user DMA */
typedef struct AddressSpace {
    int dummy;
} AddressSpace;

/* MemoryRegion initialization - no-ops */
static inline void memory_region_init(MemoryRegion *mr, void *owner,
                                     const char *name, uint64_t size)
{
    (void)owner;
    if (mr) {
        mr->name = name;
        mr->size = size;
    }
}

static inline void memory_region_init_io(MemoryRegion *mr, void *owner,
                                        const void *ops, void *opaque,
                                        const char *name, uint64_t size)
{
    (void)ops; (void)opaque;
    memory_region_init(mr, owner, name, size);
}

/* Memory region operations - stubbed */
static inline void memory_region_add_subregion(MemoryRegion *mr, hwaddr offset,
                                              MemoryRegion *subregion)
{
    (void)mr; (void)offset; (void)subregion;
}

static inline void memory_region_del_subregion(MemoryRegion *mr,
                                              MemoryRegion *subregion)
{
    (void)mr; (void)subregion;
}

/* DMA types */
typedef uint64_t dma_addr_t;

typedef enum {
    DMA_DIRECTION_TO_DEVICE = 0,
    DMA_DIRECTION_FROM_DEVICE = 1,
} DMADirection;

typedef struct MemTxAttrs {
    unsigned int dummy;
} MemTxAttrs;

#define MEMTXATTRS_UNSPECIFIED ((MemTxAttrs){.dummy = 0})

typedef enum {
    MEMTX_OK,
    MEMTX_ERROR,
    MEMTX_DECODE_ERROR,
} MemTxResult;

/* Scatter-gather list */
typedef struct ScatterGatherEntry {
    dma_addr_t base;
    dma_addr_t len;
} ScatterGatherEntry;

typedef struct QEMUSGList {
    ScatterGatherEntry *sg;
    int nsg;
    int nalloc;
    size_t size;
    AddressSpace *as;
} QEMUSGList;

#endif /* QEMU_MEMORY_H */

