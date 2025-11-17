#ifndef QEMU_BSWAP_H
#define QEMU_BSWAP_H

#include <stdint.h>

/*
 * QEMU Byte Swap / Endian Conversion Stubs
 */

uint64_t be64_to_cpu(uint64_t val);
uint32_t be32_to_cpu(uint32_t val);
uint16_t be16_to_cpu(uint16_t val);
uint64_t cpu_to_be64(uint64_t val);
uint32_t cpu_to_be32(uint32_t val);
uint16_t cpu_to_be16(uint16_t val);

#endif /* QEMU_BSWAP_H */
