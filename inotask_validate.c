/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#include "inotask_validate.h"

#include <sys/stat.h>
#include <unistd.h>

#include <string.h>

static bool same(const it_str *a, const it_str *b)
{
    return a->len == b->len && memcmp(a->s, b->s, a->len) == 0;
}

static bool task_exists(const it_config *cfg, const it_str *name)
{
    size_t i;
    for (i = 0; i < cfg->tasks.n; i++)
        if (same(&cfg->tasks.v[i].name, name)) return true;
    return false;
}

static it_cfg_errc validate_task_exec(const it_task *task)
{
    struct stat st;
    if (!task) return IT_CFG_OK;
    if (stat(task->exec.s, &st) != 0) return IT_CFG_ETASK_EXEC_MISSING;
    if (!S_ISREG(st.st_mode)) return IT_CFG_ETASK_EXEC_NOT_REGULAR;
    if (access(task->exec.s, X_OK) != 0) return IT_CFG_ETASK_EXEC_NOT_EXECUTABLE;
    return IT_CFG_OK;
}

it_cfg_errc it_validate_config_v01(const it_config *cfg)
{
    size_t i, j, k;
    it_cfg_errc rc;
    for (i = 0; i < cfg->tasks.n; i++) {
        rc = validate_task_exec(&cfg->tasks.v[i]);
        if (rc != IT_CFG_OK) return rc;
    }
    for (i = 0; i < cfg->tasks.n; i++)
        for (j = i + 1; j < cfg->tasks.n; j++)
            if (same(&cfg->tasks.v[i].name, &cfg->tasks.v[j].name))
                return IT_CFG_EDUP_TASK_NAME;

    for (i = 0; i < cfg->rules.n; i++) {
        const it_rule *r = &cfg->rules.v[i];
        for (j = i + 1; j < cfg->rules.n; j++)
            if (same(&r->name, &cfg->rules.v[j].name))
                return IT_CFG_EDUP_RULE_NAME;
        for (j = 0; j < r->run.n; j++) {
            if (!task_exists(cfg, &r->run.v[j])) return IT_CFG_ERULE_TASK_UNKNOWN;
            for (k = j + 1; k < r->run.n; k++)
                if (same(&r->run.v[j], &r->run.v[k])) return IT_CFG_EDUP_RULE_RUN;
        }
    }
    return IT_CFG_OK;
}
