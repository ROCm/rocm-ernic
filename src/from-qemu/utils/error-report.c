#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "qemu/error-report.h"

/*
 * Log level state.  Initialised lazily from ERNIC_LOG_LEVEL so that
 * anything logged before main() finishes parsing its options already
 * honours the environment; an explicit ernic_log_set_level() from the
 * command line overrides it.
 */
static ErnicLogLevel g_log_level = ERNIC_LOG_WARN;
static bool g_log_level_resolved = false;

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
    g_log_level = lvl;
    g_log_level_resolved = true;
}

ErnicLogLevel ernic_log_get_level(void)
{
    if (!g_log_level_resolved) {
        const char *env = getenv("ERNIC_LOG_LEVEL");
        ErnicLogLevel lvl;

        /* Mark resolved first: the bad-value warning below logs, and
         * that would otherwise recurse back into this function. */
        g_log_level = ERNIC_LOG_WARN;
        g_log_level_resolved = true;

        if (env && env[0] != '\0') {
            if (ernic_log_parse_level(env, &lvl)) {
                g_log_level = lvl;
            } else {
                warn_report("Ignoring invalid ERNIC_LOG_LEVEL='%s' "
                            "(expected none|error|warn|info|debug)",
                            env);
            }
        }
    }
    return g_log_level;
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
