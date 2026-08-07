/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#include "inotask_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static it_log_level g_level = IT_LOG_INFO;

static const char *level_name(it_log_level level)
{
    switch (level) {
        case IT_LOG_ERROR: return "ERROR";
        case IT_LOG_WARN: return "WARN";
        case IT_LOG_INFO: return "INFO";
        case IT_LOG_DEBUG: return "DEBUG";
        default: return "LOG";
    }
}

void it_log_set_level(it_log_level level)
{
    g_level = level;
}

static bool parse_level(const char *value, it_log_level *level)
{
    if (strcmp(value, "error") == 0) *level = IT_LOG_ERROR;
    else if (strcmp(value, "warn") == 0) *level = IT_LOG_WARN;
    else if (strcmp(value, "info") == 0) *level = IT_LOG_INFO;
    else if (strcmp(value, "debug") == 0) *level = IT_LOG_DEBUG;
    else return false;
    return true;
}

bool it_log_configure_from_env(void)
{
    const char *level = getenv("INOTASK_LOG_LEVEL");
    it_log_level parsed_level;
    if (level && *level) {
        if (!parse_level(level, &parsed_level)) {
            (void)fprintf(stderr,
                          "ERROR: invalid INOTASK_LOG_LEVEL '%s'; expected error, warn, info, or debug\n",
                          level);
            return false;
        }
        g_level = parsed_level;
    }
    return true;
}

void it_log_vmsg(it_log_level level, const char *fmt, va_list ap)
{
    if (level > g_level) return;
    (void)fprintf(stderr, "%s: ", level_name(level));
    (void)vfprintf(stderr, fmt, ap);
    (void)fputc('\n', stderr);
}

static void log_msg(it_log_level level, const char *fmt, va_list ap)
{
    it_log_vmsg(level, fmt, ap);
}

void it_log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_msg(IT_LOG_ERROR, fmt, ap);
    va_end(ap);
}

void it_log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_msg(IT_LOG_WARN, fmt, ap);
    va_end(ap);
}

void it_log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_msg(IT_LOG_INFO, fmt, ap);
    va_end(ap);
}

void it_log_debug(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_msg(IT_LOG_DEBUG, fmt, ap);
    va_end(ap);
}
