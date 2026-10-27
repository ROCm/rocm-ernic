/*
 * ernic_log.c - the server's log
 *
 * See ernic_log.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "ernic_log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

/*
 * Log level state.  Initialised lazily from ERNIC_LOG_LEVEL so that
 * anything logged before main() finishes parsing its options already
 * honours the environment; an explicit ernic_log_set_level() from the
 * command line overrides it.  Every message on every thread reads it,
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
                ernic_log(ERNIC_LOG_WARN,
                          "Ignoring invalid ERNIC_LOG_LEVEL='%s' "
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

void ernic_vlog(ErnicLogLevel lvl, const char *fmt, va_list ap)
{
    const char *prefix;
    FILE *fp = stdout;

    switch (lvl) {
    case ERNIC_LOG_ERROR:
        prefix = "ERROR";
        fp = stderr;
        break;
    case ERNIC_LOG_WARN:
        prefix = "WARN";
        fp = stderr;
        break;
    case ERNIC_LOG_INFO:
        prefix = "INFO";
        break;
    case ERNIC_LOG_DEBUG:
        prefix = "DEBUG";
        break;
    case ERNIC_LOG_NONE:
    default:
        return;
    }

    if (!ernic_log_enabled(lvl)) {
        return;
    }
    fprintf(fp, "%s: ", prefix);
    vfprintf(fp, fmt, ap);
    fprintf(fp, "\n");
    fflush(fp);
}

void ernic_log(ErnicLogLevel lvl, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ernic_vlog(lvl, fmt, ap);
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

/*
 * A zeroed limit's interval starts at 0, so on the real clock the first call
 * starts a new one, and on a test's clock starting at 0 the first call is in
 * it, with nothing logged yet: either way the first line gets through.  A
 * clock earlier than the interval's start (a test's, set back) starts a new
 * one rather than waiting for the clock to catch up.
 */
bool ernic_log_limit(ErnicLogLimit *lim, uint64_t now_ms, uint64_t *held)
{
    if (now_ms < lim->start_ms ||
        now_ms - lim->start_ms >= ERNIC_LOG_LIMIT_MS) {
        lim->start_ms = now_ms;
        lim->logged = 0;
    }

    if (lim->logged >= ERNIC_LOG_LIMIT_BURST) {
        lim->held++;
        *held = 0;
        return false;
    }

    lim->logged++;
    *held = lim->held;
    lim->held = 0;
    return true;
}
