/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INOTASK_LOG_H
#define INOTASK_LOG_H

#include <stdarg.h>

/**
 * @brief Log severity levels used by the stderr logger.
 */
typedef enum it_log_level {
    IT_LOG_ERROR = 0,
    IT_LOG_WARN = 1,
    IT_LOG_INFO = 2,
    IT_LOG_DEBUG = 3
} it_log_level;

/**
 * @brief Set the minimum severity level emitted by the logger.
 *
 * Messages below the configured threshold are ignored.
 *
 * @param level Minimum level to emit.
 */
void it_log_set_level(it_log_level level);

/**
 * @brief Write a formatted log message to stderr.
 *
 * @param level Severity of the message.
 * @param fmt `printf`-style format string.
 * @param ap Variadic argument list matching @p fmt.
 */
void it_log_vmsg(it_log_level level, const char *fmt, va_list ap);

/**
 * @brief Write an error message to stderr.
 *
 * @param fmt `printf`-style format string.
 */
void it_log_error(const char *fmt, ...);

/**
 * @brief Write a warning message to stderr.
 *
 * @param fmt `printf`-style format string.
 */
void it_log_warn(const char *fmt, ...);

/**
 * @brief Write an informational message to stderr.
 *
 * @param fmt `printf`-style format string.
 */
void it_log_info(const char *fmt, ...);

/**
 * @brief Write a debug message to stderr.
 *
 * @param fmt `printf`-style format string.
 */
void it_log_debug(const char *fmt, ...);

#endif
