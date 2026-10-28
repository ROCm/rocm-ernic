/*
 * QEMU's error reporting API
 *
 * Each prints one line through the server's log (ernic_log.h), at its own
 * level.  New code calls ernic_log() directly.
 */

/*
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_ERROR_REPORT_H
#define QEMU_ERROR_REPORT_H

#include "ernic_attributes.h"

void error_report(const char *fmt, ...) ERNIC_PRINTF_FORMAT(1, 2);
void warn_report(const char *fmt, ...) ERNIC_PRINTF_FORMAT(1, 2);
void info_report(const char *fmt, ...) ERNIC_PRINTF_FORMAT(1, 2);

#endif /* QEMU_ERROR_REPORT_H */
