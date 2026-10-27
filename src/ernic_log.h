/*
 * ernic_log.h - the server's log
 *
 * One line per message, prefixed with its level's name, to stderr for
 * errors and warnings and to stdout for the rest.  What's printed depends
 * on the log level, which --log-level or ERNIC_LOG_LEVEL sets.  QEMU's
 * reporters (qemu/error-report.h), which the QEMU-derived code and the RDMA
 * backends call, print here too.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef ERNIC_LOG_H
#define ERNIC_LOG_H

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

/*
 * Log one line at @lvl, if the log level is @lvl or above.  ERNIC_LOG_NONE
 * isn't a level a message can have, and logs nothing.
 */
void ernic_log(ErnicLogLevel lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void ernic_vlog(ErnicLogLevel lvl, const char *fmt, va_list ap)
    __attribute__((format(printf, 2, 0)));

/* One-shot startup/shutdown lines: unprefixed, printed at WARN and above. */
void ernic_startup_report(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#endif /* ERNIC_LOG_H */
