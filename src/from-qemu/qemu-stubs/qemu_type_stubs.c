/*
 * QEMU Type System Stubs for Standalone PVRDMA Device
 * 
 * This file provides minimal stub implementations of QEMU's type system
 * and object model functions needed by the PVRDMA device code.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Include QEMU headers for type definitions */
#include "hw/pci/pci_device.h"
#include "qemu/notify.h"
#include "exec/memory.h"
#include "qom/object.h"

/*
 * QEMU Type Casting Macros - Implemented as Functions
 */

void *PVRDMA_DEV(void *obj)
{
    /* In standalone mode, we directly use PVRDMADev pointer */
    return obj;
}

void *OBJECT(void *obj)
{
    /* In standalone mode, just pass through the pointer */
    return obj;
}

int PCI_FUNC(PCIDevice *dev)
{
    /* Extract function number from devfn */
    if (!dev) return 0;
    return dev->devfn & 0x07;
}

int PCI_SLOT(PCIDevice *dev)
{
    /* Extract slot number from devfn */
    if (!dev) return 0;
    return (dev->devfn >> 3) & 0x1f;
}

/*
 * Object Model Functions
 */

void *object_dynamic_cast(Object *obj, const char *typename)
{
    /* In standalone mode, we trust the caller and just return the object */
    (void)typename;
    return obj;
}

bool object_property_get_bool(Object *obj, const char *name, void *errp)
{
    /* Stub: return false for any property query */
    (void)obj; (void)name; (void)errp;
    return false;
}

/*
 * PCI Functions
 */

void pci_register_bar(PCIDevice *pci_dev, int region_num,
                      uint8_t type, MemoryRegion *memory)
{
    /* In standalone mode, BAR registration is handled by libvfio-user */
    (void)pci_dev; (void)region_num; (void)type; (void)memory;
}

/*
 * Notifier Functions
 */

void notifier_remove(Notifier *notifier)
{
    /* Stub: no-op in standalone mode */
    (void)notifier;
}

