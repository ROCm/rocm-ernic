#ifndef QEMU_CUTILS_H
#define QEMU_CUTILS_H

/*
 * QEMU C Utility Functions Stubs
 */

void pstrcpy(char *buf, int buf_size, const char *str);

/* Math utilities */
uint64_t pow2ceil(uint64_t value);
uint64_t ROUND_UP(uint64_t n, uint64_t align);
uint64_t BIT(int n);

/* Array size - implemented as function since we can't use macros properly */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#endif /* QEMU_CUTILS_H */

