/*
 * Minimal PCI stub for PVRDMA
 * Only includes what PVRDMA actually uses
 */

#ifndef QEMU_PCI_H
#define QEMU_PCI_H

#include <stdint.h>
#include "hw/pci/pci_device.h"

/* PCI BAR indices */
#define PCI_NUM_REGIONS 6

/* PCI configuration space offsets - standard */
#define PCI_INTERRUPT_PIN      0x3d
#define PCI_CONFIG_SPACE_SIZE  256
#define PCIE_CONFIG_SPACE_SIZE 4096

/* Helper function - just returns the config byte */
static inline uint8_t pci_get_byte(const uint8_t *config)
{
    return *config;
}

/* Stub - PVRDMA doesn't use PCI bus functions */
typedef struct PCIBus PCIBus;

static inline PCIBus *pci_get_bus(PCIDevice *dev)
{
    (void)dev;
    return NULL;
}

static inline int pci_bus_num(PCIBus *bus)
{
    (void)bus;
    return 0;
}

/* Build BDF (Bus/Device/Function) identifier */
#define PCI_BUILD_BDF(bus, devfn) ((((uint16_t)(bus)) << 8) | (devfn))

/* PCI BAR address space types */
#define PCI_BASE_ADDRESS_SPACE_MEMORY 0x00
#define PCI_BASE_ADDRESS_SPACE_IO     0x01

/* PCI DMA functions - implemented in vfu_compat_bridge.c */
void *pci_dma_map(PCIDevice *dev, uint64_t addr, uint64_t *len, int dir);
void pci_dma_unmap(PCIDevice *dev, void *buffer, uint64_t len, int dir,
                   uint64_t access_len);

#endif /* QEMU_PCI_H */
