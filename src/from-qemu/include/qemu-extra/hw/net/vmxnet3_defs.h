/*
 * Stub for VMXNET3 definitions
 * PVRDMA can optionally work with VMXNET3 as function 0, but we don't use this
 */

#ifndef QEMU_VMXNET3_DEFS_H
#define QEMU_VMXNET3_DEFS_H

/* Minimal stub - PVRDMA has a func0 pointer but we don't use it */
typedef struct VMXNET3State {
    int device_active;  /* Device active flag */
    struct {
        struct {
            uint8_t a[6];  /* MAC address */
        } macaddr;
    } conf;  /* Simplified - real struct has more fields */
} VMXNET3State;

#define TYPE_VMXNET3 "vmxnet3"
#define VMXNET3(obj) ((VMXNET3State *)(obj))

#endif /* QEMU_VMXNET3_DEFS_H */

