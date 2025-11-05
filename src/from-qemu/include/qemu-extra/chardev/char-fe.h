/*
 * Stub for QEMU Character Device Frontend
 * PVRDMA has a mad_chr field for MAD (Management Datagram) handling
 * We don't use this feature
 */

#ifndef QEMU_CHAR_FE_H
#define QEMU_CHAR_FE_H

typedef struct CharBackend {
    /* Stubbed - MAD interface not used */
    int dummy;
} CharBackend;

#endif /* QEMU_CHAR_FE_H */

