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
#include <string.h>
#include <strings.h>
#include <syslog.h>

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
 * The longest message formatted on the stack.  A longer one is formatted
 * into an allocation, so isn't cut short.
 */
#define LOG_MSG_INLINE 512

/*
 * Write @prefix, the message @fmt formats, then ": " and @suffix if it isn't
 * NULL, to @fp as one line, and flush it.
 *
 * The message is formatted first, and the line written and flushed under
 * stderr's lock, which every line takes, whichever stream it's for.  So no
 * other line can land in the middle of it, even where stdout and stderr are
 * the same file.  The lock is POSIX's flockfile() because it needs nothing
 * set up and can't fail; a QemuMutex aborts through ernic_log_fatal() when
 * it fails, which takes this lock.
 *
 * If the message is too long for the stack and there's no memory for it,
 * what fit is written, marked as cut short.
 */
ERNIC_PRINTF_FORMAT(4, 0)
static void log_write(FILE *fp, const char *prefix, const char *suffix,
                      const char *fmt, va_list ap)
{
    char buf[LOG_MSG_INLINE];
    char *big = NULL;
    const char *msg = buf;
    bool cut = false;
    va_list again;
    int n;

    va_copy(again, ap);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) {
        msg = "(a message that couldn't be formatted)";
    } else if ((size_t)n >= sizeof(buf)) {
        big = malloc((size_t)n + 1);
        if (big && vsnprintf(big, (size_t)n + 1, fmt, again) == n) {
            msg = big;
        } else {
            cut = true;
        }
    }
    va_end(again);

    flockfile(stderr);
    (void)fputs(prefix, fp);
    (void)fputs(msg, fp);
    if (cut) {
        (void)fputs(" [cut short]", fp);
    }
    if (suffix) {
        (void)fputs(": ", fp);
        (void)fputs(suffix, fp);
    }
    (void)fputc('\n', fp);
    (void)fflush(fp);
    funlockfile(stderr);
    free(big);
}

/* log_write() with the message's arguments as they are */
ERNIC_PRINTF_FORMAT(3, 4)
static void log_line(FILE *fp, const char *prefix, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_write(fp, prefix, NULL, fmt, ap);
    va_end(ap);
}

/*
 * Threads that make the first call together all read the environment, but
 * only the one whose compare-exchange stores the result warns about a bad
 * value, and a level that ernic_log_set_level() stored in the meantime
 * isn't overwritten.
 *
 * The warning is written as ernic_vlog() would write it, but not through
 * it, which would call back into this function.  A bad value leaves the
 * level at the default, ERNIC_LOG_WARN, so ernic_vlog() would print it.
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
                log_line(stderr, "WARN: ",
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

/*
 * Whether a message at @lvl is logged now, and if so, its line's prefix in
 * *@prefix and its stream in *@fp.
 */
static bool level_logged(ErnicLogLevel lvl, const char **prefix, FILE **fp)
{
    switch (lvl) {
    case ERNIC_LOG_ERROR:
        *prefix = "ERROR: ";
        *fp = stderr;
        break;
    case ERNIC_LOG_WARN:
        *prefix = "WARN: ";
        *fp = stderr;
        break;
    case ERNIC_LOG_INFO:
        *prefix = "INFO: ";
        *fp = stdout;
        break;
    case ERNIC_LOG_DEBUG:
        *prefix = "DEBUG: ";
        *fp = stdout;
        break;
    case ERNIC_LOG_NONE:
    default:
        return false;
    }
    return ernic_log_enabled(lvl);
}

void ernic_vlog(ErnicLogLevel lvl, const char *fmt, va_list ap)
{
    const char *prefix;
    FILE *fp;

    if (level_logged(lvl, &prefix, &fp)) {
        log_write(fp, prefix, NULL, fmt, ap);
    }
}

void ernic_log(ErnicLogLevel lvl, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ernic_vlog(lvl, fmt, ap);
    va_end(ap);
}

void ernic_log_errno(ErnicLogLevel lvl, int err, const char *fmt, ...)
{
    const char *prefix;
    FILE *fp;
    va_list ap;

    if (!level_logged(lvl, &prefix, &fp)) {
        return;
    }
    va_start(ap, fmt);
    log_write(fp, prefix, strerror(err), fmt, ap);
    va_end(ap);
}

_Noreturn void ernic_log_fatal(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_write(stderr, "FATAL: ", NULL, fmt, ap);
    va_end(ap);
    abort();
}

ErnicLogLevel ernic_log_level_from_syslog(int priority)
{
    switch (priority) {
    case LOG_EMERG:
    case LOG_ALERT:
    case LOG_CRIT:
    case LOG_ERR:
        return ERNIC_LOG_ERROR;
    case LOG_WARNING:
        return ERNIC_LOG_WARN;
    case LOG_NOTICE:
    case LOG_INFO:
        return ERNIC_LOG_INFO;
    case LOG_DEBUG:
        return ERNIC_LOG_DEBUG;
    default:
        return ERNIC_LOG_NONE;
    }
}

void ernic_startup_report(const char *fmt, ...)
{
    va_list ap;

    if (!ernic_log_enabled(ERNIC_LOG_WARN)) {
        return;
    }
    va_start(ap, fmt);
    log_write(stdout, "", NULL, fmt, ap);
    va_end(ap);
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
