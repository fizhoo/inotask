/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INOTASK_VALIDATE_H
#define INOTASK_VALIDATE_H

#include "inotask_config.h"

/**
 * @brief Perform semantic validation on a parsed configuration.
 *
 * Validation checks cross references, duplicate names, and other constraints
 * that are not enforced during tokenization or parsing.
 *
 * Current validation also checks task executable paths for existence, regular
 * file type, and execute permission.
 *
 * @param cfg Parsed configuration to validate.
 *
 * @return IT_CFG_OK if the configuration is semantically valid.
 * @return Non-zero configuration error code if validation fails.
 */
it_cfg_errc it_validate_config_v01(const it_config *cfg);

#endif
