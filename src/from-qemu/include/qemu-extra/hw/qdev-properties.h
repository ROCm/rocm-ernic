/*
 * QEMU device properties stub
 * PVRDMA uses these macros to define properties but we don't need the property system
 */

#ifndef QDEV_PROPERTIES_H
#define QDEV_PROPERTIES_H

#include <stdint.h>

/* Property structure - just a stub */
typedef struct Property {
    const char *name;
    int offset;
} Property;

/* Property definition macros - create dummy entries */
#define DEFINE_PROP_STRING(_n, _s, _f) \
    { .name = #_n, .offset = offsetof(_s, _f) }

#define DEFINE_PROP_UINT8(_n, _s, _f, _d) \
    { .name = #_n, .offset = offsetof(_s, _f) }

#define DEFINE_PROP_UINT64(_n, _s, _f, _d) \
    { .name = #_n, .offset = offsetof(_s, _f) }

#define DEFINE_PROP_INT32(_n, _s, _f, _d) \
    { .name = #_n, .offset = offsetof(_s, _f) }

#define DEFINE_PROP_CHR(_n, _s, _f) \
    { .name = #_n, .offset = offsetof(_s, _f) }

#define DEFINE_PROP_LINK(_n, _s, _f, _t, _p) \
    { .name = #_n, .offset = offsetof(_s, _f) }

#define DEFINE_PROP_END_OF_LIST() \
    { .name = NULL, .offset = 0 }

#endif /* QDEV_PROPERTIES_H */

