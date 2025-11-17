/*
 * Stub for QEMU PCIDevice
 * Minimal definitions to make PVRDMA code compile with libvfio-user
 */

#ifndef QEMU_PCI_DEVICE_H
#define QEMU_PCI_DEVICE_H

#include <stdint.h>
#include <stdbool.h>

/* Forward declarations */
typedef struct PCIDevice PCIDevice;
typedef struct PCIDeviceClass PCIDeviceClass;
typedef struct DeviceState DeviceState;
typedef struct DeviceClass DeviceClass;
typedef struct MemoryRegion MemoryRegion;
typedef struct Error Error;
typedef uint64_t hwaddr;

/* Forward declarations for our types */
typedef struct rocm_ernic_dev rocm_ernic_dev_t;
typedef struct vfu_ctx vfu_ctx_t;

/* Define DeviceState/DeviceClass fully to avoid incomplete type errors */
struct DeviceState {
    int dummy; /* Minimal - PVRDMA doesn't use this */
};

struct DeviceClass {
    int dummy; /* Minimal - PVRDMA doesn't use this */
};

/* Minimal PCIDevice structure - only fields we need */
struct PCIDevice {
    struct DeviceState qdev; /* Stubbed - required by QEMU type system */
    /* Fields used by our compatibility bridge */
    rocm_ernic_dev_t *vfu_dev; /* Back-pointer to our device */
    vfu_ctx_t *vfu_ctx;        /* libvfio-user context */
    /* Additional fields used by PVRDMA code */
    const char *name;    /* Device name */
    uint8_t devfn;       /* Device/function number */
    uint8_t config[256]; /* PCI config space */
};

/* Minimal PCIDeviceClass - for type system */
struct PCIDeviceClass {
    struct DeviceClass parent_class;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision;
    uint16_t class_id;
    const char *romfile;
};

/* PCI_DEVICE macro - cast to PCIDevice* (works due to parent_obj being first)
 */
#define PCI_DEVICE(obj)         ((PCIDevice *)(obj))
#define PCI_DEVICE_CLASS(klass) ((PCIDeviceClass *)(klass))

/* Type definitions for QEMU object model - stubbed */
#define TYPE_PCI_DEVICE "pci-device"

/* Stub for QEMU object checker macros */
#define DECLARE_OBJ_CHECKERS(InstanceType, ClassType, OBJ_NAME, TYPE_NAME) \
    /* Stubbed */

#define OBJECT_DECLARE_TYPE(InstanceType, ClassType, MODULE_OBJ_NAME) \
    typedef struct InstanceType InstanceType;                         \
    typedef struct ClassType ClassType;

#define DECLARE_INSTANCE_CHECKER(InstanceType, OBJ_NAME, TYPE_NAME) \
    /* Stubbed */

/* Device types - stubbed */
#define INTERFACE_CONVENTIONAL_PCI_DEVICE "conventional-pci-device"
#define INTERFACE_PCIE_DEVICE             "pci-express-device"

/* PCI IDs */
#define PCI_VENDOR_ID_VMWARE        0x15ad
#define PCI_DEVICE_ID_VMWARE_PVRDMA 0x0820

#endif /* QEMU_PCI_DEVICE_H */
