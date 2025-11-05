#ifndef HW_PCI_PCI_REGS_H
#define HW_PCI_PCI_REGS_H

/*
 * PCI Type System Macros - Implemented as Functions
 */

void *PVRDMA_DEV(void *obj);
void *OBJECT(void *obj);
int PCI_FUNC(struct PCIDevice *dev);
int PCI_SLOT(struct PCIDevice *dev);

/* Forward declarations */
struct Object;
struct PCIDevice;
struct MemoryRegion;
struct Notifier;

/* Object model functions */
void *object_dynamic_cast(struct Object *obj, const char *typename);
bool object_property_get_bool(struct Object *obj, const char *name, void *errp);

/* PCI functions */
void pci_register_bar(struct PCIDevice *pci_dev, int region_num,
                      uint8_t type, struct MemoryRegion *memory);

/* Notifier functions */
void notifier_remove(struct Notifier *notifier);

#endif /* HW_PCI_PCI_REGS_H */

