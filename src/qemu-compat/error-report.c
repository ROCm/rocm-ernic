/*
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <strings.h>

#include "qemu/error-report.h"

/*
 * Log level state.  Initialised lazily from ERNIC_LOG_LEVEL so that
 * anything logged before main() finishes parsing its options already
 * honours the environment; an explicit ernic_log_set_level() from the
 * command line overrides it.  Every *_report() on every thread reads it,
 * so it's atomic, and LOG_LEVEL_UNRESOLVED means ERNIC_LOG_LEVEL hasn't
 * been looked up yet.
 */
#define LOG_LEVEL_UNRESOLVED (-1)
static atomic_int g_log_level = LOG_LEVEL_UNRESOLVED;

bool ernic_log_parse_level(const char *s, ErnicLogLevel *out)
{
    static const struct {
        const char *name;
        ErnicLogLevel level;
    } levels[] = {
        {"none", ERNIC_LOG_NONE},   {"0", ERNIC_LOG_NONE},
        {"error", ERNIC_LOG_ERROR}, {"1", ERNIC_LOG_ERROR},
        {"warn", ERNIC_LOG_WARN},   {"warning", ERNIC_LOG_WARN},
        {"2", ERNIC_LOG_WARN},      {"info", ERNIC_LOG_INFO},
        {"3", ERNIC_LOG_INFO},      {"debug", ERNIC_LOG_DEBUG},
        {"4", ERNIC_LOG_DEBUG},
    };

    if (!s || s[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (!strcasecmp(s, levels[i].name)) {
            *out = levels[i].level;
            return true;
        }
    }
    return false;
}

const char *ernic_log_level_name(ErnicLogLevel lvl)
{
    switch (lvl) {
    case ERNIC_LOG_NONE:
        return "none";
    case ERNIC_LOG_ERROR:
        return "error";
    case ERNIC_LOG_WARN:
        return "warn";
    case ERNIC_LOG_INFO:
        return "info";
    case ERNIC_LOG_DEBUG:
        return "debug";
    default:
        return "unknown";
    }
}

void ernic_log_set_level(ErnicLogLevel lvl)
{
    atomic_store(&g_log_level, (int)lvl);
}

/*
 * Threads that make the first call together all read the environment, but
 * only the one whose compare-exchange stores the result warns about a bad
 * value, and a level that ernic_log_set_level() stored in the meantime
 * isn't overwritten.  (pthread_once() would deadlock instead: the warning
 * logs, which calls back into this function.)
 */
ErnicLogLevel ernic_log_get_level(void)
{
    int cur = atomic_load(&g_log_level);

    if (cur == LOG_LEVEL_UNRESOLVED) {
        const char *env = getenv("ERNIC_LOG_LEVEL");
        ErnicLogLevel lvl = ERNIC_LOG_WARN;
        bool invalid = false;

        if (env && env[0] != '\0') {
            ErnicLogLevel parsed;

            if (ernic_log_parse_level(env, &parsed)) {
                lvl = parsed;
            } else {
                invalid = true;
            }
        }

        /* On failure, cur is updated to the level another thread stored. */
        if (atomic_compare_exchange_strong(&g_log_level, &cur, (int)lvl)) {
            cur = (int)lvl;
            if (invalid) {
                warn_report("Ignoring invalid ERNIC_LOG_LEVEL='%s' "
                            "(expected none|error|warn|info|debug)",
                            env);
            }
        }
    }
    return (ErnicLogLevel)cur;
}

bool ernic_log_enabled(ErnicLogLevel lvl)
{
    return ernic_log_get_level() >= lvl;
}

void error_vreport(const char *fmt, va_list ap)
{
    fprintf(stderr, "ERROR: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

void warn_vreport(const char *fmt, va_list ap)
{
    fprintf(stderr, "WARN: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

void info_vreport(const char *fmt, va_list ap)
{
    printf("INFO: ");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
}

void error_report(const char *fmt, ...)
{
    va_list ap;

    if (!ernic_log_enabled(ERNIC_LOG_ERROR)) {
        return;
    }
    va_start(ap, fmt);
    error_vreport(fmt, ap);
    va_end(ap);
}

void warn_report(const char *fmt, ...)
{
    va_list ap;

    if (!ernic_log_enabled(ERNIC_LOG_WARN)) {
        return;
    }
    va_start(ap, fmt);
    warn_vreport(fmt, ap);
    va_end(ap);
}

void info_report(const char *fmt, ...)
{
    va_list ap;

    if (!ernic_log_enabled(ERNIC_LOG_INFO)) {
        return;
    }
    va_start(ap, fmt);
    info_vreport(fmt, ap);
    va_end(ap);
}

void ernic_startup_report(const char *fmt, ...)
{
    va_list ap;

    if (!ernic_log_enabled(ERNIC_LOG_WARN)) {
        return;
    }
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}
