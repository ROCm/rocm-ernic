/*
 * Stub for QEMU hw/hw.h
 * Provides VMState macros (we don't use state serialization)
 */

#ifndef QEMU_HW_H
#define QEMU_HW_H

/* VMState - stubbed, we don't support state save/load */
typedef struct VMStateDescription {
    const char *name;
    /* Stubbed */
} VMStateDescription;

#endif /* QEMU_HW_H */
