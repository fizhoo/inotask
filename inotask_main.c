/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include "inotask_load.h"
#include "inotask_log.h"
#include "inotask_runtime.h"

#include <errno.h>
#include <fnmatch.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t g_reap_requested = 0;

static void on_sigchld(int signo)
{
    (void)signo;
    g_reap_requested = 1;
}

/**
 * @brief Append text to a fixed-size display buffer.
 *
 * The helper truncates silently when the destination buffer is full, which is
 * acceptable for human-readable debug output.
 *
 * @param buf Destination buffer containing a NUL-terminated string.
 * @param cap Total capacity of @p buf in bytes.
 * @param text Text to append.
 */
static void append_text(char *buf, size_t cap, const char *text)
{
    size_t len;
    size_t avail;
    int wrote;
    if (!buf || !text || cap == 0) return;
    len = strlen(buf);
    if (len >= cap - 1) return;
    avail = cap - len;
    wrote = snprintf(buf + len, avail, "%s", text);
    (void)wrote;
}

static void append_sep_text(char *buf, size_t cap, bool *first, const char *text)
{
    append_text(buf, cap, *first ? text : ", ");
    if (!*first) append_text(buf, cap, text);
    *first = false;
}

/**
 * @brief Convert an internal event mask into a comma-separated display string.
 *
 * @param m Internal event mask.
 * @param buf Output buffer.
 * @param cap Capacity of @p buf in bytes.
 */
static void events_to_buf(it_event_mask m, char *buf, size_t cap)
{
    bool first = true;
    if (cap == 0) return;
    buf[0] = '\0';
    if ((m & IT_EVT_CREATE) != 0) append_sep_text(buf, cap, &first, "CREATE");
    if ((m & IT_EVT_MODIFY) != 0) append_sep_text(buf, cap, &first, "MODIFY");
    if ((m & IT_EVT_DELETE) != 0) append_sep_text(buf, cap, &first, "DELETE");
    if ((m & IT_EVT_MOVE) != 0) append_sep_text(buf, cap, &first, "MOVE");
    if ((m & IT_EVT_ATTRIB) != 0) append_sep_text(buf, cap, &first, "ATTRIB");
    if ((m & IT_EVT_CLOSE_WRITE) != 0)
        append_sep_text(buf, cap, &first, "CLOSE_WRITE");
}

/**
 * @brief Convert a vector of strings into a quoted, comma-separated display
 *        string.
 *
 * @param vec Input vector.
 * @param buf Output buffer.
 * @param cap Capacity of @p buf in bytes.
 */
static void str_vec_to_buf(const it_str_vec *vec, char *buf, size_t cap)
{
    size_t i;
    if (cap == 0) return;
    buf[0] = '\0';
    for (i = 0; i < vec->n; i++) {
        append_text(buf, cap, i == 0 ? "\"" : ", \"");
        append_text(buf, cap, vec->v[i].s);
        append_text(buf, cap, "\"");
    }
}

static void append_str_vec_quoted(const it_str_vec *vec, char *buf, size_t cap)
{
    size_t i;
    for (i = 0; i < vec->n; i++) {
        append_text(buf, cap, i == 0 ? "\"" : ", \"");
        append_text(buf, cap, vec->v[i].s);
        append_text(buf, cap, "\"");
    }
}

static void print_rule_line(const it_rule *rule)
{
    char events[64];
    char filters[256];
    char run[256];
    bool first = true;
    events_to_buf(rule->events, events, sizeof(events));
    filters[0] = '\0';
    if (rule->include.n != 0) {
        append_text(filters, sizeof(filters), "include=");
        append_text(filters, sizeof(filters), "[");
        append_str_vec_quoted(&rule->include, filters, sizeof(filters));
        append_text(filters, sizeof(filters), "]");
        first = false;
    }
    if (rule->exclude.n != 0) {
        if (!first) append_text(filters, sizeof(filters), " ");
        append_text(filters, sizeof(filters), "exclude=");
        append_text(filters, sizeof(filters), "[");
        append_str_vec_quoted(&rule->exclude, filters, sizeof(filters));
        append_text(filters, sizeof(filters), "]");
    }
    str_vec_to_buf(&rule->run, run, sizeof(run));
    printf("%-16s %-20s %-22s %-28s %s\n",
           rule->name.s, rule->watch_path.s, events, filters, run);
}

static bool str_eq_cstr(const it_str *s, const char *cstr)
{
    size_t n;
    if (!s || !cstr) return false;
    n = strlen(cstr);
    return s->len == n && memcmp(s->s, cstr, n) == 0;
}

/**
 * @brief Locate a configured task by name.
 *
 * @param cfg Loaded configuration.
 * @param name Task name to resolve.
 *
 * @return Pointer to the matching task, or NULL if no such task exists.
 */
static const it_task *find_task(const it_config *cfg, const it_str *name)
{
    size_t i;
    for (i = 0; i < cfg->tasks.n; i++)
        if (str_eq_cstr(name, cfg->tasks.v[i].name.s)) return &cfg->tasks.v[i];
    return NULL;
}

static bool any_pattern_matches(const it_str_vec *patterns, const char *name)
{
    size_t i;
    if (!patterns || !name) return false;
    for (i = 0; i < patterns->n; i++)
        if (fnmatch(patterns->v[i].s, name, 0) == 0) return true;
    return false;
}

static bool rule_name_filter_matches(const it_rule *rule, const char *entry_name)
{
    if (!rule) return false;
    if (rule->include.n == 0 && rule->exclude.n == 0) return true;
    if (!entry_name || *entry_name == '\0') return false;
    if (rule->include.n != 0 && !any_pattern_matches(&rule->include, entry_name))
        return false;
    if (rule->exclude.n != 0 && any_pattern_matches(&rule->exclude, entry_name))
        return false;
    return true;
}

/**
 * @brief Event-specific values available for task argument expansion.
 */
typedef struct it_event_vars {
    const char *watch_path;
    const char *entry_name;
    const char *full_path;
    const char *event_name;
} it_event_vars;

static bool append_bytes(char **buf, size_t *len, size_t *cap,
                         const char *src, size_t src_n)
{
    char *nv;
    size_t need;
    if (!buf || !len || !cap || (!src && src_n != 0)) return false;
    need = *len + src_n + 1;
    if (need > *cap) {
        size_t nc = *cap ? *cap : 32;
        while (nc < need) nc *= 2;
        nv = (char *)realloc(*buf, nc);
        if (!nv) return false;
        *buf = nv;
        *cap = nc;
    }
    if (src_n != 0) memcpy(*buf + *len, src, src_n);
    *len += src_n;
    (*buf)[*len] = '\0';
    return true;
}

static const char *placeholder_value(const char *name, size_t name_n,
                                     const it_event_vars *vars)
{
    if (name_n == strlen("watch_path") &&
        memcmp(name, "watch_path", name_n) == 0)
        return vars->watch_path;
    if (name_n == strlen("entry_name") &&
        memcmp(name, "entry_name", name_n) == 0)
        return vars->entry_name;
    if (name_n == strlen("full_path") &&
        memcmp(name, "full_path", name_n) == 0)
        return vars->full_path;
    if (name_n == strlen("event") &&
        memcmp(name, "event", name_n) == 0)
        return vars->event_name;
    return NULL;
}

/**
 * @brief Expand supported event placeholders inside one task argument.
 *
 * Supported placeholders are `{watch_path}`, `{entry_name}`, `{full_path}`,
 * and `{event}`. Unknown placeholders are left unchanged.
 *
 * @param templ Raw configured argument template.
 * @param vars Event values available for expansion.
 *
 * @return Newly allocated expanded string, or NULL on allocation failure.
 */
static char *expand_arg_template(const char *templ, const it_event_vars *vars)
{
    char *out = NULL;
    size_t i = 0, len = 0, cap = 0;
    if (!templ || !vars) return NULL;
    while (templ[i] != '\0') {
        size_t j;
        const char *value;
        if (templ[i] != '{') {
            if (!append_bytes(&out, &len, &cap, templ + i, 1)) {
                free(out);
                return NULL;
            }
            i++;
            continue;
        }
        j = i + 1;
        while (templ[j] != '\0' && templ[j] != '}') j++;
        if (templ[j] != '}') {
            if (!append_bytes(&out, &len, &cap, templ + i, 1)) {
                free(out);
                return NULL;
            }
            i++;
            continue;
        }
        value = placeholder_value(templ + i + 1, j - i - 1, vars);
        if (!value) {
            if (!append_bytes(&out, &len, &cap, templ + i, j - i + 1)) {
                free(out);
                return NULL;
            }
        } else if (!append_bytes(&out, &len, &cap, value, strlen(value))) {
            free(out);
            return NULL;
        }
        i = j + 1;
    }
    if (!out) {
        out = (char *)malloc(1);
        if (!out) return NULL;
        out[0] = '\0';
    }
    return out;
}

/**
 * @brief Build the argv array passed to `execv()` for a configured task.
 *
 * The returned array and its argument strings are heap-allocated. The
 * executable path stays borrowed from the task definition.
 *
 * @param task Task definition to translate into argv form.
 * @param vars Event values used to expand task argument templates.
 *
 * @return Heap-allocated argv array, or NULL on allocation failure.
 */
static char **build_exec_argv(const it_task *task, const it_event_vars *vars)
{
    char **argv;
    size_t i;
    argv = (char **)calloc(task->args.n + 2, sizeof(*argv));
    if (!argv) return NULL;
    argv[0] = task->exec.s;
    for (i = 0; i < task->args.n; i++) {
        argv[i + 1] = expand_arg_template(task->args.v[i].s, vars);
        if (!argv[i + 1]) {
            size_t j;
            for (j = 1; j < i + 1; j++) free(argv[j]);
            free(argv);
            return NULL;
        }
    }
    argv[task->args.n + 1] = NULL;
    return argv;
}

static void free_exec_argv(const it_task *task, char **argv)
{
    size_t i;
    if (!argv) return;
    for (i = 0; i < task->args.n; i++) free(argv[i + 1]);
    free(argv);
}

/**
 * @brief Log how an asynchronously launched child process finished.
 *
 * @param pid Process ID returned by `waitpid()`.
 * @param status Encoded wait status for the child.
 */
static void log_reaped_child_status(pid_t pid, int status)
{
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0) it_log_info("reaped child pid=%ld exited with status %d",
                                   (long)pid, code);
        else it_log_warn("reaped child pid=%ld exited with status %d",
                         (long)pid, code);
        return;
    }
    if (WIFSIGNALED(status)) {
        it_log_error("reaped child pid=%ld terminated by signal %d",
                     (long)pid, WTERMSIG(status));
        return;
    }
    if (WIFSTOPPED(status)) {
        it_log_warn("reaped child pid=%ld stopped by signal %d",
                    (long)pid, WSTOPSIG(status));
        return;
    }
    it_log_warn("reaped child pid=%ld ended with unrecognized wait status %d",
                (long)pid, status);
}

/**
 * @brief Reap any exited child processes without blocking the event loop.
 */
static void reap_children(void)
{
    int status;
    pid_t pid;
    g_reap_requested = 0;
    for (;;) {
        pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            log_reaped_child_status(pid, status);
            continue;
        }
        if (pid == 0) return;
        if (errno == EINTR) continue;
        if (errno != ECHILD)
            it_log_error("waitpid(WNOHANG) failed: %s", strerror(errno));
        return;
    }
}

/**
 * @brief Install the SIGCHLD handler used to trigger async child reaping.
 *
 * @return true if the handler was installed successfully.
 * @return false on failure.
 */
static bool install_sigchld_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigchld;
    if (sigemptyset(&sa.sa_mask) != 0) return false;
    sa.sa_flags = 0;
    return sigaction(SIGCHLD, &sa, NULL) == 0;
}

/**
 * @brief Launch a configured task process.
 *
 * Tasks always run asynchronously so the event loop can keep processing new
 * filesystem activity without blocking on child completion.
 *
 * @param task Task definition to execute.
 */
static void launch_task(const it_task *task, const it_event_vars *vars)
{
    pid_t pid;
    char **argv;
    argv = build_exec_argv(task, vars);
    if (!argv) {
        it_log_error("cannot allocate argv for task %s", task->name.s);
        return;
    }
    pid = fork();
    if (pid < 0) {
        it_log_error("cannot fork for task %s", task->name.s);
        free_exec_argv(task, argv);
        return;
    }
    if (pid == 0) {
        execv(task->exec.s, argv);
        it_log_error("task %s failed: execv(%s): %s",
                     task->name.s, task->exec.s, strerror(errno));
        _exit(127);
    }
    it_log_info("launched task %s pid=%ld", task->name.s, (long)pid);
    free_exec_argv(task, argv);
}

/**
 * @brief Match a normalized event against configured rules and run any tasks
 *        referenced by matching rules.
 *
 * @param cfg Loaded configuration.
 * @param target Runtime watch target that produced the event.
 * @param mask Normalized internal event mask.
 * @param name Optional entry name reported by inotify.
 */
static void dispatch_event(const it_config *cfg, const it_watch_target *target,
                           it_event_mask mask, const char *name)
{
    size_t i;
    char events[64];
    char *full_path;
    bool any = false;
    it_event_vars vars;
    size_t watch_len;
    size_t name_len;
    events_to_buf(mask, events, sizeof(events));
    watch_len = strlen(target->path.s);
    name_len = (name && *name) ? strlen(name) : 0;
    full_path = (char *)malloc(watch_len + (name_len ? 1 + name_len : 0) + 1);
    if (!full_path) {
        it_log_error("cannot allocate full path for event under %s", target->path.s);
        return;
    }
    if (name_len != 0) {
        memcpy(full_path, target->path.s, watch_len);
        full_path[watch_len] = '/';
        memcpy(full_path + watch_len + 1, name, name_len);
        full_path[watch_len + 1 + name_len] = '\0';
    } else {
        memcpy(full_path, target->path.s, watch_len + 1);
    }
    vars.watch_path = target->path.s;
    vars.entry_name = (name && *name) ? name : "";
    vars.full_path = full_path;
    vars.event_name = events;
    if (name && *name) it_log_info("event path=%s entry=%s events=%s",
                                   target->path.s, name, events);
    else it_log_info("event path=%s events=%s", target->path.s, events);
    for (i = 0; i < cfg->rules.n; i++) {
        const it_rule *rule = &cfg->rules.v[i];
        size_t j;
        if (!str_eq_cstr(&rule->watch_path, target->path.s)) continue;
        if ((rule->events & mask) == 0) continue;
        if (!rule_name_filter_matches(rule, vars.entry_name)) continue;
        any = true;
        it_log_info("rule %s matched", rule->name.s);
        for (j = 0; j < rule->run.n; j++) {
            const it_task *task = find_task(cfg, &rule->run.v[j]);
            if (!task) continue;
            it_log_info("task %s -> %s",
                        task->name.s, task->exec.s);
            launch_task(task, &vars);
        }
    }
    if (!any) it_log_info("no rule matched");
    free(full_path);
}

int main(int argc, char **argv)
{
    it_config cfg;
    it_runtime_plan plan;
    it_runtime_session session;
    size_t i;
    char buf[4096]
    ;
    it_log_set_level(IT_LOG_INFO);
    if (argc != 2) { it_log_error("usage: %s <config-file>", argv[0]); return 2; }
    if (!it_load_config_file(argv[1], &cfg)) return 1;
    it_runtime_plan_init(&plan);
    it_runtime_session_init(&session);
    if (!it_runtime_plan_build(&cfg, &plan)) {
        it_log_error("cannot build runtime watch plan");
        it_config_free(&cfg);
        return 1;
    }
    printf("Config loaded successfully.\nWatches: %zu  Tasks: %zu  Rules: %zu\n",
           cfg.watches.n, cfg.tasks.n, cfg.rules.n);
    printf("\nWATCHES\n");
    printf("%-20s %s\n", "PATH", "EVENTS");
    printf("%-20s %s\n", "--------------------", "----------------------------");
    for (i = 0; i < cfg.watches.n; i++) {
        char events[64];
        events_to_buf(cfg.watches.v[i].events, events, sizeof(events));
        printf("%-20s %s\n", cfg.watches.v[i].path.s, events);
    }
    printf("\nTASKS\n");
    printf("%-16s %-24s %s\n", "NAME", "EXEC", "ARGS");
    printf("%-16s %-24s %s\n", "----------------",
           "------------------------", "------------------------------");
    for (i = 0; i < cfg.tasks.n; i++) {
        char args_buf[256];
        str_vec_to_buf(&cfg.tasks.v[i].args, args_buf, sizeof(args_buf));
        printf("%-16s %-24s %s\n",
               cfg.tasks.v[i].name.s,
               cfg.tasks.v[i].exec.s,
               args_buf);
    }
    printf("\nRULES\n");
    printf("%-16s %-20s %-22s %-28s %s\n",
           "NAME", "WATCH", "EVENTS", "FILTERS", "RUN");
    printf("%-16s %-20s %-22s %-28s %s\n", "----------------", "--------------------",
           "----------------------", "----------------------------",
           "------------------------------");
    for (i = 0; i < cfg.rules.n; i++) print_rule_line(&cfg.rules.v[i]);
    printf("\nRUNTIME WATCH TARGETS\n");
    printf("%-20s %-10s %s\n", "PATH", "WATCH_IDX", "NOTE");
    printf("%-20s %-10s %s\n", "--------------------", "----------",
           "------------------------------");
    for (i = 0; i < plan.targets.n; i++) {
        printf("%-20s %-10zu %s\n",
               plan.targets.v[i].path.s,
               plan.targets.v[i].spec_index,
               "base path watch");
    }
    if (!it_runtime_session_open(&cfg, &plan, &session)) {
        it_log_error("cannot open inotify watches");
        it_runtime_plan_free(&plan);
        it_config_free(&cfg);
        return 1;
    }
    if (!install_sigchld_handler()) {
        it_log_error("cannot install SIGCHLD handler: %s", strerror(errno));
        it_runtime_session_free(&session);
        it_runtime_plan_free(&plan);
        it_config_free(&cfg);
        return 1;
    }
    it_log_info("watching for filesystem events; press Ctrl-C to stop");
    for (;;) {
        ssize_t nread;
        size_t off = 0;
        if (g_reap_requested) reap_children();
        nread = read(session.fd, buf, sizeof(buf));
        if (nread < 0) {
            if (errno == EINTR) {
                if (g_reap_requested) reap_children();
                continue;
            }
            it_log_error("inotify read failed: %s", strerror(errno));
            break;
        }
        if (nread == 0) {
            it_log_warn("inotify stream closed");
            break;
        }
        while (off + sizeof(struct inotify_event) <= (size_t)nread) {
            const struct inotify_event *ev =
                (const struct inotify_event *)(const void *)(buf + off);
            size_t ev_size = sizeof(*ev) + ev->len;
            const it_watch_target *target =
                NULL;
            it_event_mask mask;
            if (ev_size > (size_t)nread - off) {
                it_log_warn("truncated inotify event record; stopping event parsing");
                break;
            }
            target = it_runtime_session_target_for_wd(&plan, &session, ev->wd);
            if (!target) {
                off += ev_size;
                continue;
            }
            mask = it_runtime_event_mask_from_inotify(ev->mask);
            if (mask != 0)
                dispatch_event(&cfg, target, mask, ev->len ? ev->name : "");
            off += ev_size;
        }
        if (g_reap_requested) reap_children();
    }
    reap_children();
    it_runtime_session_free(&session);
    it_runtime_plan_free(&plan);
    it_config_free(&cfg);
    return 0;
}
