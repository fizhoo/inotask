/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#include "inotask_runtime.h"

#include <sys/inotify.h>
#include <unistd.h>

#include <stdlib.h>
#include <string.h>

static size_t next_cap(size_t cap) { return cap ? cap * 2 : 8; }

/**
 * @brief Copy a C string into an owned runtime string.
 *
 * @param out Destination string object.
 * @param s Source string.
 *
 * @return true on success.
 * @return false if allocation fails or inputs are invalid.
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

void it_runtime_plan_init(it_runtime_plan *plan)
{
    memset(plan, 0, sizeof(*plan));
}

void it_runtime_plan_free(it_runtime_plan *plan)
{
    size_t i;
    if (!plan) return;
    for (i = 0; i < plan->targets.n; i++) str_free(&plan->targets.v[i].path);
    free(plan->targets.v);
    memset(plan, 0, sizeof(*plan));
}

/**
 * @brief Append a unique runtime watch target to a plan.
 *
 * This helper suppresses duplicate `(spec_index, path)` pairs so the runtime
 * installs at most one watch for each derived target.
 *
 * @param plan Runtime plan to modify.
 * @param path Runtime watch path.
 * @param spec_index Index of the source watch specification.
 *
 * @return true on success.
 * @return false if allocation fails.
 */
static bool push_target(it_runtime_plan *plan, const char *path, size_t spec_index)
{
    it_watch_target *nv;
    size_t nc;
    size_t i;
    it_watch_target *slot;
    for (i = 0; i < plan->targets.n; i++) {
        if (plan->targets.v[i].spec_index == spec_index &&
            strcmp(plan->targets.v[i].path.s, path) == 0)
            return true;
    }
    if (plan->targets.n == plan->targets.cap) {
        nc = next_cap(plan->targets.cap);
        nv = (it_watch_target *)realloc(plan->targets.v, nc * sizeof(*nv));
        if (!nv) return false;
        plan->targets.v = nv;
        plan->targets.cap = nc;
    }
    slot = &plan->targets.v[plan->targets.n];
    memset(slot, 0, sizeof(*slot));
    if (!str_set(&slot->path, path)) return false;
    slot->spec_index = spec_index;
    plan->targets.n++;
    return true;
}

bool it_runtime_plan_build(const it_config *cfg, it_runtime_plan *plan)
{
    size_t i;
    if (!cfg || !plan) return false;
    it_runtime_plan_free(plan);
    it_runtime_plan_init(plan);
    for (i = 0; i < cfg->watches.n; i++) {
        if (!push_target(plan, cfg->watches.v[i].path.s, i)) {
            it_runtime_plan_free(plan);
            return false;
        }
    }
    return true;
}

void it_runtime_session_init(it_runtime_session *session)
{
    memset(session, 0, sizeof(*session));
    session->fd = -1;
}

void it_runtime_session_free(it_runtime_session *session)
{
    if (!session) return;
    if (session->fd >= 0) close(session->fd);
    free(session->bindings.v);
    memset(session, 0, sizeof(*session));
    session->fd = -1;
}

/**
 * @brief Convert an internal event mask into the inotify flags needed to watch
 *        for the same logical events.
 *
 * @param mask Internal event bitmask.
 *
 * @return Inotify bitmask suitable for `inotify_add_watch()`.
 */
uint32_t it_runtime_inotify_mask(it_event_mask mask)
{
    uint32_t out = 0;
    if ((mask & IT_EVT_CREATE) != 0) out |= IN_CREATE | IN_MOVED_TO;
    if ((mask & IT_EVT_MODIFY) != 0) out |= IN_MODIFY;
    if ((mask & IT_EVT_DELETE) != 0) out |= IN_DELETE | IN_DELETE_SELF;
    if ((mask & IT_EVT_MOVE) != 0) out |= IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF;
    if ((mask & IT_EVT_ATTRIB) != 0) out |= IN_ATTRIB;
    if ((mask & IT_EVT_CLOSE_WRITE) != 0) out |= IN_CLOSE_WRITE;
    return out;
}

/**
 * @brief Normalize raw inotify flags into the internal event model.
 *
 * Several inotify flags collapse into a single logical event such as
 * `IT_EVT_CREATE` or `IT_EVT_MOVE`.
 *
 * @param mask Raw inotify event flags.
 *
 * @return Internal event bitmask representing the observed event.
 */
it_event_mask it_runtime_event_mask_from_inotify(uint32_t mask)
{
    it_event_mask out = 0;
    if ((mask & (IN_CREATE | IN_MOVED_TO)) != 0) out |= IT_EVT_CREATE;
    if ((mask & IN_MODIFY) != 0) out |= IT_EVT_MODIFY;
    if ((mask & (IN_DELETE | IN_DELETE_SELF)) != 0) out |= IT_EVT_DELETE;
    if ((mask & (IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF)) != 0) out |= IT_EVT_MOVE;
    if ((mask & IN_ATTRIB) != 0) out |= IT_EVT_ATTRIB;
    if ((mask & IN_CLOSE_WRITE) != 0) out |= IT_EVT_CLOSE_WRITE;
    return out;
}

/**
 * @brief Record the association between an inotify watch descriptor and a
 *        runtime watch target.
 *
 * @param session Runtime session to modify.
 * @param wd Inotify watch descriptor.
 * @param target_index Index of the associated runtime target.
 *
 * @return true on success.
 * @return false if allocation fails.
 */
static bool push_binding(it_runtime_session *session, int wd, size_t target_index)
{
    it_watch_binding *nv;
    size_t nc;
    if (session->bindings.n == session->bindings.cap) {
        nc = next_cap(session->bindings.cap);
        nv = (it_watch_binding *)realloc(session->bindings.v, nc * sizeof(*nv));
        if (!nv) return false;
        session->bindings.v = nv;
        session->bindings.cap = nc;
    }
    session->bindings.v[session->bindings.n].wd = wd;
    session->bindings.v[session->bindings.n].target_index = target_index;
    session->bindings.n++;
    return true;
}

bool it_runtime_session_open(const it_config *cfg, const it_runtime_plan *plan,
                             it_runtime_session *session)
{
    size_t i;
    uint32_t mask;
    int wd;
    if (!cfg || !plan || !session) return false;
    it_runtime_session_free(session);
    it_runtime_session_init(session);
    session->fd = inotify_init1(0);
    if (session->fd < 0) return false;
    for (i = 0; i < plan->targets.n; i++) {
        const it_watch_target *target = &plan->targets.v[i];
        mask = it_runtime_inotify_mask(cfg->watches.v[target->spec_index].events);
        wd = inotify_add_watch(session->fd, target->path.s, mask);
        if (wd < 0 || !push_binding(session, wd, i)) {
            it_runtime_session_free(session);
            return false;
        }
    }
    return true;
}

const it_watch_target *it_runtime_session_target_for_wd(
    const it_runtime_plan *plan, const it_runtime_session *session, int wd)
{
    size_t i;
    if (!plan || !session) return NULL;
    for (i = 0; i < session->bindings.n; i++) {
        if (session->bindings.v[i].wd == wd)
            return &plan->targets.v[session->bindings.v[i].target_index];
    }
    return NULL;
}
