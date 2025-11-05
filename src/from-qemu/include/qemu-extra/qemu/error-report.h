/*
 * QEMU error reporting stub
 * Redirects to standard printf/fprintf
 */

#ifndef QEMU_ERROR_REPORT_H
#define QEMU_ERROR_REPORT_H

#include <stdio.h>
#include <stdarg.h>

/* Simple implementations that just print to stderr/stdout */

static inline int error_vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stderr, fmt, ap);
}

static inline int error_printf(const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = vfprintf(stderr, fmt, ap);
    va_end(ap);
    return ret;
}

static inline void error_vreport(const char *fmt, va_list ap)
{
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
}

static inline void warn_vreport(const char *fmt, va_list ap)
{
    fprintf(stderr, "Warning: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
}

static inline void info_vreport(const char *fmt, va_list ap)
{
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
}

static inline void error_report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    error_vreport(fmt, ap);
    va_end(ap);
}

static inline void warn_report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    warn_vreport(fmt, ap);
    va_end(ap);
}

static inline void info_report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    info_vreport(fmt, ap);
    va_end(ap);
}

/* Location-aware variants - just ignore location */
static inline void error_report_once_cond(int *printed, const char *fmt, ...)
{
    if (!*printed) {
        va_list ap;
        va_start(ap, fmt);
        error_vreport(fmt, ap);
        va_end(ap);
        *printed = 1;
    }
}

static inline void warn_report_once_cond(int *printed, const char *fmt, ...)
{
    if (!*printed) {
        va_list ap;
        va_start(ap, fmt);
        warn_vreport(fmt, ap);
        va_end(ap);
        *printed = 1;
    }
}

/* Global state - not used but stubbed for compatibility */
static bool message_with_timestamp = false;
static bool error_with_guestname = false;
static const char *error_guest_name = NULL;

#endif /* QEMU_ERROR_REPORT_H */

