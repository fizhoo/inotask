/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INOTASK_PARSER_H
#define INOTASK_PARSER_H

#include "inotask_config.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Parse error with source location and human-readable detail.
 */
typedef struct it_parse_error {
    size_t line;
    size_t col;
    char *detail;
} it_parse_error;

/**
 * @brief Parse a configuration buffer into an `it_config`.
 *
 * This function performs syntactic parsing only. Callers typically validate
 * the resulting configuration separately after a successful parse.
 *
 * @param buf Input buffer containing configuration text.
 * @param len Number of bytes available in @p buf.
 * @param cfg Output configuration object that receives parsed values.
 * @param err Output parse error structure populated on failure.
 *
 * @return true if parsing succeeded.
 * @return false if the input is syntactically invalid or memory allocation
 *         fails.
 */
bool it_parse_config(const char *buf, size_t len, it_config *cfg,
                     it_parse_error *err);

/**
 * @brief Free memory owned by a parse error structure.
 *
 * @param err Parse error structure to clean up.
 */
void it_parse_error_free(it_parse_error *err);

#endif
