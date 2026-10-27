/*
 * QEMU's error reporting API, printed through the server's log (ernic_log.h)
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdarg.h>

#include "ernic_log.h"
#include "qemu/error-report.h"

void error_report(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ernic_vlog(ERNIC_LOG_ERROR, fmt, ap);
    va_end(ap);
}

void warn_report(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ernic_vlog(ERNIC_LOG_WARN, fmt, ap);
    va_end(ap);
}

void info_report(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ernic_vlog(ERNIC_LOG_INFO, fmt, ap);
    va_end(ap);
}
