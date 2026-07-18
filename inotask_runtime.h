/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INOTASK_RUNTIME_H
#define INOTASK_RUNTIME_H

#include "inotask_config.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Concrete runtime watch target derived from a watch specification.
 *
 * Targets are derived from merged watch paths in the validated configuration.
 */
typedef struct it_watch_target {
    it_str path;
    size_t spec_index;
} it_watch_target;

/**
 * @brief Dynamic array of runtime watch targets.
 */
typedef struct it_watch_target_vec {
    it_watch_target *v;
    size_t n, cap;
} it_watch_target_vec;

/**
 * @brief Runtime watch installation plan derived from the configuration.
 */
typedef struct it_runtime_plan {
    it_watch_target_vec targets;
} it_runtime_plan;

/**
 * @brief Mapping between an inotify watch descriptor and a runtime target.
 */
typedef struct it_watch_binding {
    int wd;
    size_t target_index;
} it_watch_binding;

/**
 * @brief Dynamic array of watch-descriptor bindings.
 */
typedef struct it_watch_binding_vec {
    it_watch_binding *v;
    size_t n, cap;
} it_watch_binding_vec;

/**
 * @brief Open runtime watch session backed by an inotify file descriptor.
 */
typedef struct it_runtime_session {
    int fd;
    it_watch_binding_vec bindings;
} it_runtime_session;

/**
 * @brief Initialize a runtime plan to an empty state.
 *
 * @param plan Runtime plan to initialize.
 */
void it_runtime_plan_init(it_runtime_plan *plan);

/**
 * @brief Free memory owned by a runtime plan.
 *
 * @param plan Runtime plan to release.
 */
void it_runtime_plan_free(it_runtime_plan *plan);

/**
 * @brief Build a runtime watch plan from a validated configuration.
 *
 * Rules that share the same watched path are already merged into derived watch
 * specifications by the configuration layer. This function translates those
 * derived watches into runtime watch targets.
 *
 * @param cfg Configuration to translate into runtime watch targets.
 * @param plan Output runtime plan.
 *
 * @return true on success.
 * @return false if allocation fails or input pointers are invalid.
 */
bool it_runtime_plan_build(const it_config *cfg, it_runtime_plan *plan);

/**
 * @brief Initialize a runtime watch session to an empty state.
 *
 * @param session Runtime session to initialize.
 */
void it_runtime_session_init(it_runtime_session *session);

/**
 * @brief Release all resources owned by a runtime watch session.
 *
 * This closes the inotify file descriptor when it is open.
 *
 * @param session Runtime session to release.
 */
void it_runtime_session_free(it_runtime_session *session);

/**
 * @brief Open inotify watches for every target in a runtime plan.
 *
 * @param cfg Configuration whose watch event masks will be installed.
 * @param plan Runtime plan describing the paths to watch.
 * @param session Output runtime session.
 *
 * @return true if all watches were installed successfully.
 * @return false if inotify setup or memory allocation fails.
 */
bool it_runtime_session_open(const it_config *cfg, const it_runtime_plan *plan,
                             it_runtime_session *session);

/**
 * @brief Look up the runtime watch target associated with an inotify watch id.
 *
 * @param plan Runtime plan containing watch targets.
 * @param session Runtime session containing watch descriptor bindings.
 * @param wd Inotify watch descriptor to resolve.
 *
 * @return Pointer to the matching runtime watch target, or NULL if not found.
 */
const it_watch_target *it_runtime_session_target_for_wd(
    const it_runtime_plan *plan, const it_runtime_session *session, int wd);

/**
 * @brief Convert an internal event mask to an inotify event mask.
 *
 * @param mask Internal event mask.
 *
 * @return Equivalent inotify event bitmask suitable for `inotify_add_watch()`.
 */
uint32_t it_runtime_inotify_mask(it_event_mask mask);

/**
 * @brief Convert an inotify event mask to an internal event mask.
 *
 * @param mask Inotify event bitmask.
 *
 * @return Equivalent internal event mask used by rule matching.
 */
it_event_mask it_runtime_event_mask_from_inotify(uint32_t mask);

#endif
