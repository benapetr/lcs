// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_LOG_H
#define LCS_LOG_H

#include <stdbool.h>

void lcs_log_open(const char *ident, bool foreground, int verbosity, bool syslog_enabled, bool timestamp_enabled);
void lcs_log_close(void);
void lcs_log_info(const char *fmt, ...);
void lcs_log_warn(const char *fmt, ...);
void lcs_log_error(const char *fmt, ...);
void lcs_log_debug(const char *fmt, ...);
void lcs_log_debug2(const char *fmt, ...);
void lcs_log_debug3(const char *fmt, ...);

#endif
