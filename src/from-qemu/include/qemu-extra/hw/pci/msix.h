/*
 * Stub for QEMU MSI-X functions
 * libvfio-user handles MSI-X, so these are mostly no-ops
 */

#ifndef QEMU_MSIX_H
#define QEMU_MSIX_H

#include <stdint.h>
#include <stdbool.h>

/* Forward declarations */
typedef struct PCIDevice PCIDevice;
typedef struct MemoryRegion MemoryRegion;

/*
 * MSI-X functions - stubbed out since libvfio-user handles interrupts
 *
 * In QEMU, these manage MSI-X table and PBA regions in BARs.
 * With libvfio-user, vfu_irq_trigger() is used instead.
 */

/* Initialize MSI-X - no-op for us */
static inline int msix_init(PCIDevice *dev, unsigned short nentries,
                            MemoryRegion *table_bar, uint8_t table_bar_nr,
                            unsigned table_offset, MemoryRegion *pba_bar,
                            uint8_t pba_bar_nr, unsigned pba_offset,
                            uint8_t cap_pos, void **errp)
{
    /* libvfio-user handles MSI-X setup */
    (void)dev;
    (void)nentries;
    (void)table_bar;
    (void)table_bar_nr;
    (void)table_offset;
    (void)pba_bar;
    (void)pba_bar_nr;
    (void)pba_offset;
    (void)cap_pos;
    (void)errp;
    return 0; /* Success */
}

/* Cleanup MSI-X - no-op */
static inline void msix_uninit(PCIDevice *dev, MemoryRegion *table_bar,
                               MemoryRegion *pba_bar)
{
    (void)dev;
    (void)table_bar;
    (void)pba_bar;
}

/* Mark vector as used - no-op */
static inline int msix_vector_use(PCIDevice *dev, unsigned vector)
{
    (void)dev;
    (void)vector;
    return 0;
}

/* Mark vector as unused - no-op */
static inline void msix_vector_unuse(PCIDevice *dev, unsigned vector)
{
    (void)dev;
    (void)vector;
}

/* Check if MSI-X is enabled - always true for us */
static inline bool msix_enabled(PCIDevice *dev)
{
    (void)dev;
    return true; /* We always use MSI-X */
}

/* Send MSI-X interrupt - handled by our post_interrupt() in bridge */
static inline void msix_notify(PCIDevice *dev, unsigned vector)
{
    /* This is called from pvrdma.h's inline post_interrupt()
     * But we've replaced that function in vfu_compat_bridge.h
     * So this should never be called. Just stub it out.
     */
    (void)dev;
    (void)vector;
}

#endif /* QEMU_MSIX_H */
