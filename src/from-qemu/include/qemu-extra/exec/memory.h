/*
 * Stub for QEMU Memory API
 * PVRDMA uses MemoryRegion for BARs, but libvfio-user handles this
 */

#ifndef QEMU_MEMORY_H
#define QEMU_MEMORY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Forward declarations to avoid conflicts */
#ifndef QEMU_TYPEDEFS_H
#define QEMU_TYPEDEFS_H
typedef uint64_t hwaddr;
typedef uint64_t dma_addr_t;
#endif

/* Minimal MemoryRegion stub */
struct MemoryRegion {
    /* Stubbed - libvfio-user manages our BARs */
    const char *name;
    uint64_t addr;
    uint64_t size;
};
typedef struct MemoryRegion MemoryRegion;

/* AddressSpace - stubbed, we use libvfio-user DMA */
struct AddressSpace {
    int dummy;
};
typedef struct AddressSpace AddressSpace;

/* Endianness enum */
enum device_endian {
    DEVICE_LITTLE_ENDIAN,
    DEVICE_BIG_ENDIAN,
    DEVICE_NATIVE_ENDIAN,
};

/* Memory region operations - callback structure */
struct MemoryRegionOps {
    uint64_t (*read)(void *opaque, uint64_t addr, unsigned size);
    void (*write)(void *opaque, uint64_t addr, uint64_t data, unsigned size);
    enum device_endian endianness;
    /* Many other fields in real QEMU but we don't need them */
    struct {
        unsigned min_access_size;
        unsigned max_access_size;
    } impl;
    struct {
        unsigned min_access_size;
        unsigned max_access_size;
    } valid;
};
typedef struct MemoryRegionOps MemoryRegionOps;

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
                                        const MemoryRegionOps *ops, void *opaque,
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
enum DMADirection {
    DMA_DIRECTION_TO_DEVICE = 0,
    DMA_DIRECTION_FROM_DEVICE = 1,
};
typedef enum DMADirection DMADirection;

struct MemTxAttrs {
    unsigned int dummy;
};
typedef struct MemTxAttrs MemTxAttrs;

#define MEMTXATTRS_UNSPECIFIED ((MemTxAttrs){.dummy = 0})

enum MemTxResult {
    MEMTX_OK,
    MEMTX_ERROR,
    MEMTX_DECODE_ERROR,
};
typedef enum MemTxResult MemTxResult;

/* Scatter-gather list */
struct ScatterGatherEntry {
    uint64_t base;
    uint64_t len;
};
typedef struct ScatterGatherEntry ScatterGatherEntry;

struct QEMUSGList {
    ScatterGatherEntry *sg;
    int nsg;
    int nalloc;
    size_t size;
    AddressSpace *as;
};
typedef struct QEMUSGList QEMUSGList;

#endif /* QEMU_MEMORY_H */

