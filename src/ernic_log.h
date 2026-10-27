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
#include <stdint.h>

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

/*
 * A limit on how often one message is logged, for a message that something
 * outside the process (a guest, a mesh peer, a client) can make us log as
 * often as it likes.  Unlimited, a guest looping on a refused command fills
 * the disk.
 *
 * Each place that logs such a message keeps its own limit, zeroed to start,
 * in the object the message is about (the device, not a static), and asks
 * ernic_log_limit() before each line.  At most ERNIC_LOG_LIMIT_BURST lines
 * get through in each ERNIC_LOG_LIMIT_MS, as with Linux's printk_ratelimit().
 * The rest are counted, and the first line let through after them says how
 * many there were, so the log still shows how big a flood was, though only
 * once the message is logged again.
 *
 * A limit isn't thread safe: keep it where one thread uses it, or under the
 * lock of the object that holds it.
 */
#define ERNIC_LOG_LIMIT_BURST 10u
#define ERNIC_LOG_LIMIT_MS    UINT64_C(5000)

typedef struct {
    uint64_t start_ms; /* when the current interval began */
    uint32_t logged;   /* lines let through in it */
    uint64_t held;     /* lines held back since the last one let through */
} ErnicLogLimit;

/*
 * Whether one more line may be logged now, at @now_ms on the caller's
 * monotonic clock.  If so, sets *@held to how many lines were held back
 * since the last one let through, for the caller to report before this one,
 * and starts counting again from 0.  If not, counts this one and sets
 * *@held to 0.
 */
bool ernic_log_limit(ErnicLogLimit *lim, uint64_t now_ms, uint64_t *held);

#endif /* ERNIC_LOG_H */
