/*
 * QEMU unit conversions stub
 */

#ifndef QEMU_UNITS_H
#define QEMU_UNITS_H

/* Memory unit conversions */
#define KiB (1024ULL)
#define MiB (1024ULL * KiB)
#define GiB (1024ULL * MiB)

/* Time scale conversions */
#define SCALE_MS 1000000ULL    /* milliseconds to nanoseconds */
#define SCALE_US 1000ULL        /* microseconds to nanoseconds */

#endif /* QEMU_UNITS_H */

