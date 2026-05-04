// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>
#include <time.h>

static bool g_foreground;
static bool g_syslog_enabled;
static bool g_timestamp_enabled;
static int g_verbosity;

void lcs_log_open(const char *ident, bool foreground, int verbosity, bool syslog_enabled, bool timestamp_enabled)
{
    g_foreground = foreground;
    g_syslog_enabled = syslog_enabled;
    g_timestamp_enabled = timestamp_enabled;
    g_verbosity = verbosity;
    if (g_syslog_enabled)
        openlog(ident, LOG_PID | LOG_NDELAY, LOG_DAEMON);
}

void lcs_log_close(void)
{
    if (g_syslog_enabled)
        closelog();
}

static void log_v(int priority, const char *label, const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    if (g_syslog_enabled)
        vsyslog(priority, fmt, ap);

    if (g_foreground)
    {
        if (g_timestamp_enabled)
        {
            time_t now = time(NULL);
            struct tm tm_now;
            char time_buf[16] = "00:00:00";
            if (localtime_r(&now, &tm_now))
                strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm_now);

            fprintf(stderr, "%s: %s: ", time_buf, label);
        } else
            fprintf(stderr, "%s: ", label);
        vfprintf(stderr, fmt, copy);
        fputc('\n', stderr);
    }
    va_end(copy);
}

void lcs_log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_v(LOG_INFO, "info", fmt, ap);
    va_end(ap);
}

void lcs_log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_v(LOG_WARNING, "warn", fmt, ap);
    va_end(ap);
}

void lcs_log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_v(LOG_ERR, "error", fmt, ap);
    va_end(ap);
}

void lcs_log_debug(const char *fmt, ...)
{
    if (g_verbosity < 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    log_v(LOG_DEBUG, "debug", fmt, ap);
    va_end(ap);
}

void lcs_log_debug2(const char *fmt, ...)
{
    if (g_verbosity < 2)
        return;
    va_list ap;
    va_start(ap, fmt);
    log_v(LOG_DEBUG, "debug2", fmt, ap);
    va_end(ap);
}

void lcs_log_debug3(const char *fmt, ...)
{
    if (g_verbosity < 3)
        return;
    va_list ap;
    va_start(ap, fmt);
    log_v(LOG_DEBUG, "debug3", fmt, ap);
    va_end(ap);
}
