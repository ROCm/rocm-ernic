/*
 * QEMU Utility Function Stubs for Standalone PVRDMA Device
 *
 * This file provides minimal stub implementations of QEMU utility functions
 * needed by the PVRDMA device code. These stubs allow the QEMU PVRDMA code
 * to link and run in a standalone libvfio-user environment.
 */

#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <poll.h>
#include <time.h>
#include <endian.h>
#include <errno.h>

/*
 * Atomic Operations
 * For standalone mode, we use simple non-atomic operations since we're
 * single-threaded in the device logic.
 */

void __qatomic_set_impl(int *ptr, int val)
{
    *ptr = val;
}

int __qatomic_read_impl(const int *ptr)
{
    return *ptr;
}

void __qatomic_inc_impl(int *ptr)
{
    (*ptr)++;
}

void __qatomic_dec_impl(int *ptr)
{
    (*ptr)--;
}

void __qatomic_add_impl(int *ptr, int val)
{
    *ptr += val;
}

void __qatomic_sub_impl(int *ptr, int val)
{
    *ptr -= val;
}

/*
 * Thread Operations
 */

typedef struct QemuThread {
    pthread_t thread;
} QemuThread;

typedef struct QemuMutex {
    pthread_mutex_t lock;
} QemuMutex;

typedef struct QemuCond {
    pthread_cond_t cond;
} QemuCond;

int qemu_thread_create(QemuThread *thread, const char *name,
                       void *(*start_routine)(void *), void *arg, int mode)
{
    (void)name;
    (void)mode;
    return pthread_create(&thread->thread, NULL, start_routine, arg);
}

void qemu_thread_exit(void *retval)
{
    pthread_exit(retval);
}

void qemu_thread_join(QemuThread *thread)
{
    pthread_join(thread->thread, NULL);
}

/*
 * Bit Manipulation
 */

unsigned long *bitmap_new(int nbits)
{
    int len = (nbits + 63) / 64;
    return calloc(len, sizeof(unsigned long));
}

void bitmap_free(unsigned long *bitmap)
{
    free(bitmap);
}

void set_bit(int nr, unsigned long *addr)
{
    addr[nr / 64] |= (1UL << (nr % 64));
}

void clear_bit(int nr, unsigned long *addr)
{
    addr[nr / 64] &= ~(1UL << (nr % 64));
}

int test_bit(int nr, const unsigned long *addr)
{
    return (addr[nr / 64] & (1UL << (nr % 64))) != 0;
}

int find_first_zero_bit(const unsigned long *addr, unsigned long size)
{
    for (unsigned long i = 0; i < size; i++) {
        if (!test_bit(i, addr)) {
            return i;
        }
    }
    return size;
}

/*
 * Endian Conversion
 */

uint64_t be64_to_cpu(uint64_t val)
{
    return be64toh(val);
}

uint32_t be32_to_cpu(uint32_t val)
{
    return be32toh(val);
}

uint16_t be16_to_cpu(uint16_t val)
{
    return be16toh(val);
}

uint64_t cpu_to_be64(uint64_t val)
{
    return htobe64(val);
}

uint32_t cpu_to_be32(uint32_t val)
{
    return htobe32(val);
}

uint16_t cpu_to_be16(uint16_t val)
{
    return htobe16(val);
}

/*
 * String Utilities
 */

void pstrcpy(char *buf, int buf_size, const char *str)
{
    int len = strlen(str);
    if (len >= buf_size) {
        len = buf_size - 1;
    }
    memcpy(buf, str, len);
    buf[len] = '\0';
}

/*
 * Math Utilities
 */

/* Round up to next power of 2 */
uint64_t pow2ceil(uint64_t value)
{
    if (value == 0) {
        return 1;
    }
    value--;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    value |= value >> 32;
    value++;
    return value;
}

/* Round up to multiple of align */
uint64_t ROUND_UP(uint64_t n, uint64_t align)
{
    return ((n + align - 1) / align) * align;
}

/* Get bit at position */
uint64_t BIT(int n)
{
    return 1ULL << n;
}

/* Get array size */
size_t ARRAY_SIZE_impl(size_t n)
{
    return n;
}

/*
 * Polling
 */

int64_t qemu_clock_get_ns(int type)
{
    struct timespec ts;
    (void)type;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int qemu_poll_ns(struct pollfd *fds, nfds_t nfds, int64_t timeout_ns)
{
    int timeout_ms = timeout_ns / 1000000;
    return poll(fds, nfds, timeout_ms);
}

/*
 * Character Device Stubs (for MAD multiplexer - not used in standalone mode)
 */

typedef struct CharBackend CharBackend;

bool qemu_chr_fe_backend_connected(CharBackend *be)
{
    (void)be;
    return false; /* No character backend in standalone mode */
}

int qemu_chr_fe_set_handlers(CharBackend *be, void *fd_can_read, void *fd_read,
                             void *fd_event, void *be_change, void *opaque,
                             void *context, bool sync_state)
{
    (void)be;
    (void)fd_can_read;
    (void)fd_read;
    (void)fd_event;
    (void)be_change;
    (void)opaque;
    (void)context;
    (void)sync_state;
    return 0;
}

void qemu_chr_fe_disconnect(CharBackend *be)
{
    (void)be;
}

int qemu_chr_fe_read_all(CharBackend *be, uint8_t *buf, int len)
{
    (void)be;
    (void)buf;
    (void)len;
    return -ENOTSUP; /* No character backend in standalone mode */
}

int qemu_chr_fe_write(CharBackend *be, const uint8_t *buf, int len)
{
    (void)be;
    (void)buf;
    (void)len;
    return -ENOTSUP; /* No character backend in standalone mode */
}

/*
 * QAPI Event Stubs (for GID status notifications - not used in standalone)
 */

void qapi_event_send_rdma_gid_status_changed(const char *netdev,
                                             bool gid_status_changed,
                                             uint64_t subnet_prefix,
                                             uint64_t interface_id)
{
    (void)netdev;
    (void)gid_status_changed;
    (void)subnet_prefix;
    (void)interface_id;
    /* No-op in standalone mode */
}

/*
 * Lock Guard Stub - not used as a function, implemented as macro in thread.h
 */

/*
 * Backend Functions
 * Note: rdma_rm_init and rdma_rm_fini are implemented in rdma_rm.c
 */

void rdma_backend_destroy(void *backend_dev)
{
    /* TODO: Implement proper cleanup of RDMA backend resources */
    (void)backend_dev;
}

/*
 * DMA Mapping Wrappers
 * These forward to pci_dma_map/unmap which are implemented in
 * vfu_compat_bridge.c
 */

/* Forward declare PCIDevice from hw/pci/pci_device.h */
typedef struct PCIDevice PCIDevice;

extern void *pci_dma_map(PCIDevice *dev, uint64_t addr, uint64_t *plen,
                         int dir);
extern void pci_dma_unmap(PCIDevice *dev, void *buffer, uint64_t len, int dir,
                          uint64_t access_len);

/* When ERNIC_DEBUG_DMA_MAP is truthy in the environment, log each map. */
static int rdma_pci_dma_map_debug(void)
{
    static int cached = -1;

    if (cached >= 0) {
        return cached;
    }
    const char *v = getenv("ERNIC_DEBUG_DMA_MAP");

    if (v && v[0] != '\0' && v[0] != '0') {
        cached = 1;
    } else {
        cached = 0;
    }
    return cached;
}

void *rdma_pci_dma_map(void *dev, uint64_t addr, uint64_t len)
{
    uint64_t plen = len;
    void *result = pci_dma_map((PCIDevice *)dev, addr, &plen, 0);

    if (rdma_pci_dma_map_debug()) {
        uint64_t result_as_int = (uint64_t)(uintptr_t)result;

        printf("rdma_pci_dma_map: dev=%p guest=%#" PRIx64 " len=%#" PRIx64
               " host=%p (as uint64=%#" PRIx64 ")\n",
               dev, (uint64_t)addr, (uint64_t)len, result, result_as_int);
        fflush(stdout);
    }

    return result;
}

void rdma_pci_dma_unmap(void *dev, void *buffer, uint64_t len)
{
    pci_dma_unmap((PCIDevice *)dev, buffer, len, 0, len);
}
