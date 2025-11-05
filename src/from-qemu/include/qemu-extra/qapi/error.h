/*
 * QEMU error API stub
 * We don't use QEMU's Error type in standalone mode
 */

#ifndef QAPI_ERROR_H
#define QAPI_ERROR_H

#include <stdint.h>
#include <stdbool.h>

/* Error type - just a stub */
typedef struct Error Error;

/* Error append hint - no-op in standalone */
static inline void error_append_hint(Error **errp, const char *fmt, ...)
{
    (void)errp;
    (void)fmt;
}

/* Error setg - no-op in standalone */
static inline void error_setg(Error **errp, const char *fmt, ...)
{
    (void)errp;
    (void)fmt;
}

static inline void error_setg_errno(Error **errp, int os_errno, const char *fmt, ...)
{
    (void)errp;
    (void)os_errno;
    (void)fmt;
}

/* Error propagate - no-op */
static inline void error_propagate(Error **dst_errp, Error *local_err)
{
    (void)dst_errp;
    (void)local_err;
}

#endif /* QAPI_ERROR_H */

