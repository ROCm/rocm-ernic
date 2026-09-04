/*
 * QEMU error reporting stub
 * Redirects to standard printf/fprintf
 */

#ifndef QEMU_ERROR_REPORT_H
#define QEMU_ERROR_REPORT_H

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>

/*
 * Runtime log level.  Set from the --log-level command line option or
 * the ERNIC_LOG_LEVEL environment variable; defaults to ERNIC_LOG_WARN
 * so per-operation INFO chatter stays out of the steady-state logs.
 */
typedef enum {
    ERNIC_LOG_NONE = 0,
    ERNIC_LOG_ERROR,
    ERNIC_LOG_WARN,
    ERNIC_LOG_INFO,
    ERNIC_LOG_DEBUG,
} ErnicLogLevel;

bool ernic_log_parse_level(const char *s, ErnicLogLevel *out);
const char *ernic_log_level_name(ErnicLogLevel lvl);
void ernic_log_set_level(ErnicLogLevel lvl);
ErnicLogLevel ernic_log_get_level(void);
bool ernic_log_enabled(ErnicLogLevel lvl);

/* One-shot startup/shutdown lines: unprefixed, printed at WARN and above. */
void ernic_startup_report(const char *fmt, ...);

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

/* Declarations - implementations in error-report.c */
void error_report(const char *fmt, ...);
void warn_report(const char *fmt, ...);
void info_report(const char *fmt, ...);
void error_vreport(const char *fmt, va_list ap);
void warn_vreport(const char *fmt, va_list ap);
void info_vreport(const char *fmt, va_list ap);

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
static bool message_with_timestamp __attribute__((unused)) = false;
static bool error_with_guestname __attribute__((unused)) = false;
static const char *error_guest_name __attribute__((unused)) = NULL;

#endif /* QEMU_ERROR_REPORT_H */
