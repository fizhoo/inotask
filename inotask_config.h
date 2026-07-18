/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INOTASK_CONFIG_H
#define INOTASK_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Owned string value used throughout the configuration model.
 *
 * The string data is heap-allocated and tracked with an explicit length.
 */
typedef struct it_str {
    char *s;
    size_t len;
} it_str;

/**
 * @brief Bitmask describing one or more filesystem events.
 */
typedef uint32_t it_event_mask;

/**
 * @brief Filesystem events understood by the configuration and runtime layers.
 */
enum {
    IT_EVT_CREATE = 1u << 0,
    IT_EVT_MODIFY = 1u << 1,
    IT_EVT_DELETE = 1u << 2,
    IT_EVT_MOVE   = 1u << 3,
    IT_EVT_ATTRIB = 1u << 4,
    IT_EVT_CLOSE_WRITE = 1u << 5
};

/**
 * @brief Dynamic array of owned strings.
 */
typedef struct it_str_vec { it_str *v; size_t n, cap; } it_str_vec;

/**
 * @brief Derived watch specification built from configured rules.
 *
 * Each watch groups a path with the combined event mask required by all rules
 * that reference that path.
 */
typedef struct it_watch { it_str path; it_event_mask events; } it_watch;

/**
 * @brief Dynamic array of derived watch specifications.
 */
typedef struct it_watch_vec { it_watch *v; size_t n, cap; } it_watch_vec;

/**
 * @brief Executable task definition referenced by one or more rules.
 *
 * The runtime launches @p exec with an argv built as
 * `[ exec, args..., NULL ]`.
 */
typedef struct it_task {
    it_str name;
    it_str exec;
    it_str_vec args;
} it_task;

/**
 * @brief Dynamic array of configured tasks.
 */
typedef struct it_task_vec { it_task *v; size_t n, cap; } it_task_vec;

/**
 * @brief Rule that binds a watch path and event mask to task names.
 *
 * Include and exclude patterns are optional glob-style filename filters
 * evaluated against the event entry name at runtime.
 */
typedef struct it_rule {
    it_str name;
    it_str watch_path;
    it_event_mask events;
    it_str_vec include;
    it_str_vec exclude;
    it_str_vec run;
} it_rule;

/**
 * @brief Dynamic array of configured rules.
 */
typedef struct it_rule_vec { it_rule *v; size_t n, cap; } it_rule_vec;

/**
 * @brief Top-level configuration object.
 *
 * The configuration stores derived watches, declared tasks, and declared rules.
 */
typedef struct it_config {
    it_watch_vec watches;
    it_task_vec tasks;
    it_rule_vec rules;
} it_config;

/**
 * @brief Configuration-level error codes.
 */
typedef enum it_cfg_errc {
    IT_CFG_OK = 0,
    IT_CFG_ENOMEM,
    IT_CFG_EWATCH_PATH_NOT_ABS,
    IT_CFG_EWATCH_EVENTS_EMPTY,
    IT_CFG_ETASK_EXEC_NOT_ABS,
    IT_CFG_ETASK_EXEC_MISSING,
    IT_CFG_ETASK_EXEC_NOT_REGULAR,
    IT_CFG_ETASK_EXEC_NOT_EXECUTABLE,
    IT_CFG_ERULE_EVENTS_EMPTY,
    IT_CFG_ERULE_RUN_EMPTY,
    IT_CFG_ERULE_WATCH_UNKNOWN,
    IT_CFG_ERULE_TASK_UNKNOWN,
    IT_CFG_EDUP_WATCH_PATH,
    IT_CFG_EDUP_TASK_NAME,
    IT_CFG_EDUP_RULE_NAME,
    IT_CFG_EDUP_RULE_RUN
} it_cfg_errc;

/**
 * @brief Detailed configuration error value.
 */
typedef struct it_cfg_error { it_cfg_errc code; char *detail; } it_cfg_error;

/**
 * @brief Initialize a configuration object to an empty state.
 *
 * @param cfg Configuration object to initialize.
 */
void it_config_init(it_config *cfg);

/**
 * @brief Free all memory owned by a configuration object.
 *
 * After this call, @p cfg is reset to the same empty state produced by
 * `it_config_init()`.
 *
 * @param cfg Configuration object to release.
 */
void it_config_free(it_config *cfg);

/**
 * @brief Add a derived watch specification to the configuration.
 *
 * @param cfg Configuration object to modify.
 * @param path Absolute watch path.
 * @param events Event mask associated with the watch path.
 *
 * @return IT_CFG_OK on success.
 * @return IT_CFG_ENOMEM if allocation fails.
 * @return IT_CFG_EWATCH_PATH_NOT_ABS if @p path is not absolute.
 * @return IT_CFG_EWATCH_EVENTS_EMPTY if @p events is empty.
 */
it_cfg_errc it_config_add_watch(it_config *cfg, const char *path,
                                it_event_mask events);

/**
 * @brief Add a task definition to the configuration.
 *
 * @param cfg Configuration object to modify.
 * @param name Task name referenced by rules.
 * @param exec Absolute path to the executable file.
 * @param args Argument strings appended after @p exec.
 * @param args_n Number of entries in @p args.
 * @return IT_CFG_OK on success.
 * @return IT_CFG_ENOMEM if allocation fails.
 * @return IT_CFG_ETASK_EXEC_NOT_ABS if @p exec is not absolute.
 *
 * Existence, file-type, and execute-permission checks are performed later by
 * the validation layer after parsing is complete.
 */
it_cfg_errc it_config_add_task(it_config *cfg, const char *name,
                               const char *exec, const char *const *args,
                               size_t args_n);

/**
 * @brief Add a rule definition to the configuration.
 *
 * This call also merges the rule's watch path into the derived watch set so
 * the runtime can install the minimum number of filesystem watches.
 *
 * @param cfg Configuration object to modify.
 * @param name Rule name.
 * @param watch_path Absolute path watched by the rule.
 * @param events Event mask that triggers the rule.
 * @param include_patterns Optional glob-style filename patterns that must
 *        match the event entry name.
 * @param include_n Number of entries in @p include_patterns.
 * @param exclude_patterns Optional glob-style filename patterns that must not
 *        match the event entry name.
 * @param exclude_n Number of entries in @p exclude_patterns.
 * @param run_tasks Array of task names to execute when the rule matches.
 * @param run_n Number of entries in @p run_tasks.
 *
 * @return IT_CFG_OK on success.
 * @return IT_CFG_ENOMEM if allocation fails.
 * @return IT_CFG_EWATCH_PATH_NOT_ABS if @p watch_path is not absolute.
 * @return IT_CFG_ERULE_EVENTS_EMPTY if @p events is empty.
 * @return IT_CFG_ERULE_RUN_EMPTY if @p run_tasks is empty.
 */
it_cfg_errc it_config_add_rule(it_config *cfg, const char *name,
                               const char *watch_path, it_event_mask events,
                               const char *const *include_patterns,
                               size_t include_n,
                               const char *const *exclude_patterns,
                               size_t exclude_n,
                               const char *const *run_tasks, size_t run_n);

/**
 * @brief Check whether a path string is absolute.
 *
 * @param s Path string to examine.
 *
 * @return true if the path begins with '/'.
 * @return false otherwise.
 */
bool it_path_is_absolute(const char *s);

/**
 * @brief Convert a configuration error code to a readable string.
 *
 * @param c Configuration error code.
 *
 * @return Constant string describing the error code.
 *
 * The returned strings are intended for human-readable diagnostics.
 */
const char *it_cfg_errc_str(it_cfg_errc c);

#endif
