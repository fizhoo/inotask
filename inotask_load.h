/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INOTASK_LOAD_H
#define INOTASK_LOAD_H

#include "inotask_config.h"

#include <stdbool.h>

/**
 * @brief Load, parse, and validate a configuration file.
 *
 * Reads the configuration file from disk, parses its contents into the
 * provided configuration object, and performs validation before runtime
 * startup.
 *
 * Validation currently includes semantic cross-reference checks and task
 * executable-path checks.
 *
 * @param path Path to the configuration file.
 * @param cfg Output configuration object that receives the parsed data.
 *
 * @return true if the configuration was loaded successfully.
 * @return false if reading, parsing, or validation failed.
 */
bool it_load_config_file(const char *path, it_config *cfg);

#endif
