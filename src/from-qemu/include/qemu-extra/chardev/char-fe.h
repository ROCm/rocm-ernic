/*
 * Stub for QEMU Character Device Frontend
 * PVRDMA has a mad_chr field for MAD (Management Datagram) handling
 * We don't use this feature
 */

#ifndef QEMU_CHAR_FE_H
#define QEMU_CHAR_FE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct CharBackend {
    /* Stubbed - MAD interface not used */
    int dummy;
} CharBackend;

/* Character device function declarations */
bool qemu_chr_fe_backend_connected(CharBackend *be);
int qemu_chr_fe_set_handlers(CharBackend *be, void *fd_can_read, void *fd_read,
                             void *fd_event, void *be_change, void *opaque,
                             void *context, bool sync_state);
void qemu_chr_fe_disconnect(CharBackend *be);
int qemu_chr_fe_read_all(CharBackend *be, uint8_t *buf, int len);
int qemu_chr_fe_write(CharBackend *be, const uint8_t *buf, int len);

#endif /* QEMU_CHAR_FE_H */
