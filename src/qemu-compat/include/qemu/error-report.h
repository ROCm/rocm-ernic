/*
 * QEMU error reporting stub
 * Redirects to standard printf/fprintf
 */

/*
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
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
void ernic_startup_report(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* Declarations - implementations in error-report.c */
void error_report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void warn_report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void info_report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void error_vreport(const char *fmt, va_list ap)
    __attribute__((format(printf, 1, 0)));
void warn_vreport(const char *fmt, va_list ap)
    __attribute__((format(printf, 1, 0)));
void info_vreport(const char *fmt, va_list ap)
    __attribute__((format(printf, 1, 0)));

/* Global state - not used but stubbed for compatibility */
static bool message_with_timestamp __attribute__((unused)) = false;
static bool error_with_guestname __attribute__((unused)) = false;
static const char *error_guest_name __attribute__((unused)) = NULL;

#endif /* QEMU_ERROR_REPORT_H */
