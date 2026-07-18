/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#include "inotask_config.h"

#include <stdlib.h>
#include <string.h>

static size_t next_cap(size_t cap) { return cap ? cap * 2 : 8; }

/**
 * @brief Copy a NUL-terminated C string into an owned `it_str`.
 *
 * This helper allocates new storage and copies the full string, including the
 * terminating NUL byte. The destination is updated only on success.
 *
 * @param out Destination string object.
 * @param s Source C string.
 *
 * @return true if the copy succeeds.
 * @return false if the input is invalid or allocation fails.
 */
static bool str_set(it_str *out, const char *s)
{
    size_t n;
    char *p;
    if (!out || !s) return false;
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (!p) return false;
    memcpy(p, s, n + 1);
    out->s = p;
    out->len = n;
    return true;
}

static void str_free(it_str *s)
{
    if (!s) return;
    free(s->s);
    s->s = NULL;
    s->len = 0;
}

/**
 * @brief Append a string to a dynamic string vector.
 *
 * The vector grows automatically as needed. Appended values are stored as
 * owned string copies.
 *
 * @param v Vector to extend.
 * @param s Source string to append.
 *
 * @return IT_CFG_OK on success.
 * @return IT_CFG_ENOMEM if allocation or copying fails.
 */
static it_cfg_errc str_vec_push(it_str_vec *v, const char *s)
{
    it_str *nv;
    size_t nc;
    if (v->n == v->cap) {
        nc = next_cap(v->cap);
        nv = (it_str *)realloc(v->v, nc * sizeof(*nv));
        if (!nv) return IT_CFG_ENOMEM;
        v->v = nv;
        v->cap = nc;
    }
    if (!str_set(&v->v[v->n], s)) return IT_CFG_ENOMEM;
    v->n++;
    return IT_CFG_OK;
}

static void str_vec_free(it_str_vec *v)
{
    size_t i;
    for (i = 0; i < v->n; i++) str_free(&v->v[i]);
    free(v->v);
    memset(v, 0, sizeof(*v));
}

void it_config_init(it_config *cfg) { memset(cfg, 0, sizeof(*cfg)); }

void it_config_free(it_config *cfg)
{
    size_t i;
    if (!cfg) return;
    for (i = 0; i < cfg->watches.n; i++) str_free(&cfg->watches.v[i].path);
    free(cfg->watches.v);
    for (i = 0; i < cfg->tasks.n; i++) {
        str_free(&cfg->tasks.v[i].name);
        str_free(&cfg->tasks.v[i].exec);
        str_vec_free(&cfg->tasks.v[i].args);
    }
    free(cfg->tasks.v);
    for (i = 0; i < cfg->rules.n; i++) {
        str_free(&cfg->rules.v[i].name);
        str_free(&cfg->rules.v[i].watch_path);
        str_vec_free(&cfg->rules.v[i].include);
        str_vec_free(&cfg->rules.v[i].exclude);
        str_vec_free(&cfg->rules.v[i].run);
    }
    free(cfg->rules.v);
    it_config_init(cfg);
}

bool it_path_is_absolute(const char *s) { return s && s[0] == '/'; }

/**
 * @brief Compare an owned `it_str` against a NUL-terminated C string.
 *
 * @param a Owned string value.
 * @param b C string value.
 *
 * @return true if both strings have the same byte length and contents.
 * @return false otherwise.
 */
static bool same_cstr(const it_str *a, const char *b)
{
    size_t n;
    if (!a || !b) return false;
    n = strlen(b);
    return a->len == n && memcmp(a->s, b, n) == 0;
}

it_cfg_errc it_config_add_watch(it_config *cfg, const char *path,
                                it_event_mask events)
{
    it_watch *w;
    it_watch *nv;
    size_t nc;
    if (!it_path_is_absolute(path)) return IT_CFG_EWATCH_PATH_NOT_ABS;
    if (!events) return IT_CFG_EWATCH_EVENTS_EMPTY;
    if (cfg->watches.n == cfg->watches.cap) {
        nc = next_cap(cfg->watches.cap);
        nv = (it_watch *)realloc(cfg->watches.v, nc * sizeof(*nv));
        if (!nv) return IT_CFG_ENOMEM;
        cfg->watches.v = nv;
        cfg->watches.cap = nc;
    }
    w = &cfg->watches.v[cfg->watches.n];
    memset(w, 0, sizeof(*w));
    if (!str_set(&w->path, path)) return IT_CFG_ENOMEM;
    w->events = events;
    cfg->watches.n++;
    return IT_CFG_OK;
}

it_cfg_errc it_config_add_task(it_config *cfg, const char *name,
                               const char *exec, const char *const *args,
                               size_t args_n)
{
    it_task *t;
    it_task *nv;
    it_cfg_errc rc;
    size_t i;
    size_t nc;
    if (!it_path_is_absolute(exec)) return IT_CFG_ETASK_EXEC_NOT_ABS;
    if (cfg->tasks.n == cfg->tasks.cap) {
        nc = next_cap(cfg->tasks.cap);
        nv = (it_task *)realloc(cfg->tasks.v, nc * sizeof(*nv));
        if (!nv) return IT_CFG_ENOMEM;
        cfg->tasks.v = nv;
        cfg->tasks.cap = nc;
    }
    t = &cfg->tasks.v[cfg->tasks.n];
    memset(t, 0, sizeof(*t));
    if (!str_set(&t->name, name)) return IT_CFG_ENOMEM;
    if (!str_set(&t->exec, exec)) { str_free(&t->name); return IT_CFG_ENOMEM; }
    for (i = 0; i < args_n; i++) {
        rc = str_vec_push(&t->args, args[i]);
        if (rc != IT_CFG_OK) {
            str_free(&t->name); str_free(&t->exec); str_vec_free(&t->args); return rc;
        }
    }
    cfg->tasks.n++;
    return IT_CFG_OK;
}

it_cfg_errc it_config_add_rule(it_config *cfg, const char *name,
                               const char *watch_path, it_event_mask events,
                               const char *const *include_patterns,
                               size_t include_n,
                               const char *const *exclude_patterns,
                               size_t exclude_n,
                               const char *const *run_tasks, size_t run_n)
{
    it_rule *r;
    it_rule *nv;
    it_watch *wv;
    it_cfg_errc rc;
    size_t i, nc;
    bool found_watch = false;
    if (!it_path_is_absolute(watch_path)) return IT_CFG_EWATCH_PATH_NOT_ABS;
    if (!events) return IT_CFG_ERULE_EVENTS_EMPTY;
    if (!run_tasks || run_n == 0) return IT_CFG_ERULE_RUN_EMPTY;
    if (cfg->rules.n == cfg->rules.cap) {
        nc = next_cap(cfg->rules.cap);
        nv = (it_rule *)realloc(cfg->rules.v, nc * sizeof(*nv));
        if (!nv) return IT_CFG_ENOMEM;
        cfg->rules.v = nv;
        cfg->rules.cap = nc;
    }
    /**
     * Merge the rule's watch path into the derived watch set so the runtime
     * can install a single watch per path with the union of required events.
     */
    for (i = 0; i < cfg->watches.n; i++) {
        wv = &cfg->watches.v[i];
        if (same_cstr(&wv->path, watch_path)) {
            wv->events |= events;
            found_watch = true;
            break;
        }
    }
    if (!found_watch) {
        rc = it_config_add_watch(cfg, watch_path, events);
        if (rc != IT_CFG_OK) return rc;
    }

    r = &cfg->rules.v[cfg->rules.n];
    memset(r, 0, sizeof(*r));
    if (!str_set(&r->name, name)) return IT_CFG_ENOMEM;
    if (!str_set(&r->watch_path, watch_path)) { str_free(&r->name); return IT_CFG_ENOMEM; }
    r->events = events;
    for (i = 0; i < include_n; i++) {
        rc = str_vec_push(&r->include, include_patterns[i]);
        if (rc != IT_CFG_OK) {
            str_free(&r->name); str_free(&r->watch_path);
            str_vec_free(&r->include); str_vec_free(&r->exclude);
            str_vec_free(&r->run); return rc;
        }
    }
    for (i = 0; i < exclude_n; i++) {
        rc = str_vec_push(&r->exclude, exclude_patterns[i]);
        if (rc != IT_CFG_OK) {
            str_free(&r->name); str_free(&r->watch_path);
            str_vec_free(&r->include); str_vec_free(&r->exclude);
            str_vec_free(&r->run); return rc;
        }
    }
    for (i = 0; i < run_n; i++) {
        rc = str_vec_push(&r->run, run_tasks[i]);
        if (rc != IT_CFG_OK) {
            str_free(&r->name); str_free(&r->watch_path);
            str_vec_free(&r->include); str_vec_free(&r->exclude);
            str_vec_free(&r->run); return rc;
        }
    }
    cfg->rules.n++;
    return IT_CFG_OK;
}

const char *it_cfg_errc_str(it_cfg_errc c)
{
    switch (c) {
        case IT_CFG_OK: return "OK";
        case IT_CFG_ENOMEM: return "out of memory";
        case IT_CFG_EWATCH_PATH_NOT_ABS: return "watch path is not absolute";
        case IT_CFG_EWATCH_EVENTS_EMPTY: return "watch events list is empty";
        case IT_CFG_ETASK_EXEC_NOT_ABS: return "task executable path is not absolute";
        case IT_CFG_ETASK_EXEC_MISSING: return "task executable path does not exist";
        case IT_CFG_ETASK_EXEC_NOT_REGULAR: return "task executable path is not a regular file";
        case IT_CFG_ETASK_EXEC_NOT_EXECUTABLE: return "task executable path is not executable";
        case IT_CFG_ERULE_EVENTS_EMPTY: return "rule events list is empty";
        case IT_CFG_ERULE_RUN_EMPTY: return "rule run list is empty";
        case IT_CFG_ERULE_WATCH_UNKNOWN: return "rule references unknown watch";
        case IT_CFG_ERULE_TASK_UNKNOWN: return "rule references unknown task";
        case IT_CFG_EDUP_WATCH_PATH: return "duplicate watch path";
        case IT_CFG_EDUP_TASK_NAME: return "duplicate task name";
        case IT_CFG_EDUP_RULE_NAME: return "duplicate rule name";
        case IT_CFG_EDUP_RULE_RUN: return "duplicate task in rule run list";
        default: return "unknown configuration error";
    }
}
