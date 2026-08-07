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
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <time.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t g_reap_requested = 0;
static volatile sig_atomic_t g_stop_requested = 0;

static void on_sigchld(int signo)
{
    (void)signo;
    g_reap_requested = 1;
}

static void on_stop(int signo)
{
    (void)signo;
    g_stop_requested = 1;
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
        first = false;
    }
    if (rule->settle_ms != IT_RULE_SETTLE_MS_DEFAULT) {
        char settle[32];
        if (!first) append_text(filters, sizeof(filters), " ");
        (void)snprintf(settle, sizeof(settle), "settle_ms=%u",
                       (unsigned)rule->settle_ms);
        append_text(filters, sizeof(filters), settle);
    }
    str_vec_to_buf(&rule->run, run, sizeof(run));
    printf("%-16s %-20s %-22s %-28s %s\n",
           rule->name.s, rule->watch_path.s, events, filters, run);
}

static void print_config_summary(const it_config *cfg,
                                 const it_runtime_plan *plan)
{
    size_t item_index;
    printf("Config loaded successfully.\nWatches: %zu  Tasks: %zu  Rules: %zu\n",
           cfg->watches.n, cfg->tasks.n, cfg->rules.n);
    printf("\nWATCHES\n");
    printf("%-20s %s\n", "PATH", "EVENTS");
    printf("%-20s %s\n", "--------------------", "----------------------------");
    for (item_index = 0; item_index < cfg->watches.n; item_index++) {
        char events[64];
        events_to_buf(cfg->watches.v[item_index].events, events, sizeof(events));
        printf("%-20s %s\n", cfg->watches.v[item_index].path.s, events);
    }
    printf("\nTASKS\n");
    printf("%-16s %-24s %s\n", "NAME", "EXEC", "ARGS");
    printf("%-16s %-24s %s\n", "----------------",
           "------------------------", "------------------------------");
    for (item_index = 0; item_index < cfg->tasks.n; item_index++) {
        char args_buf[256];
        str_vec_to_buf(&cfg->tasks.v[item_index].args, args_buf, sizeof(args_buf));
        printf("%-16s %-24s %s\n",
               cfg->tasks.v[item_index].name.s,
               cfg->tasks.v[item_index].exec.s,
               args_buf);
    }
    printf("\nRULES\n");
    printf("%-16s %-20s %-22s %-28s %s\n",
           "NAME", "WATCH", "EVENTS", "FILTERS/POLICY", "RUN");
    printf("%-16s %-20s %-22s %-28s %s\n", "----------------", "--------------------",
           "----------------------", "----------------------------",
           "------------------------------");
    for (item_index = 0; item_index < cfg->rules.n; item_index++)
        print_rule_line(&cfg->rules.v[item_index]);
    printf("\nRUNTIME WATCH TARGETS\n");
    printf("%-20s %-10s %s\n", "PATH", "WATCH_IDX", "NOTE");
    printf("%-20s %-10s %s\n", "--------------------", "----------",
           "------------------------------");
    for (item_index = 0; item_index < plan->targets.n; item_index++) {
        printf("%-20s %-10zu %s\n",
               plan->targets.v[item_index].path.s,
               plan->targets.v[item_index].spec_index,
               "base path watch");
    }
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

typedef struct it_pending_event {
    size_t rule_index;
    it_event_mask events;
    char *entry_name;
    char *full_path;
    uint64_t due_ms;
} it_pending_event;

typedef struct it_pending_event_vec {
    it_pending_event *v;
    size_t n;
    size_t cap;
} it_pending_event_vec;

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static char *dup_cstr(const char *src)
{
    size_t len;
    char *copy;
    if (!src) return NULL;
    len = strlen(src);
    copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, src, len + 1);
    return copy;
}

static void pending_event_free(it_pending_event *pending)
{
    if (!pending) return;
    free(pending->entry_name);
    free(pending->full_path);
    memset(pending, 0, sizeof(*pending));
}

static void pending_event_vec_init(it_pending_event_vec *pending)
{
    memset(pending, 0, sizeof(*pending));
}

static void pending_event_vec_free(it_pending_event_vec *pending)
{
    size_t item_index;
    if (!pending) return;
    for (item_index = 0; item_index < pending->n; item_index++)
        pending_event_free(&pending->v[item_index]);
    free(pending->v);
    memset(pending, 0, sizeof(*pending));
}

static bool pending_event_vec_push(it_pending_event_vec *pending,
                                   size_t rule_index, it_event_mask events,
                                   const char *entry_name,
                                   const char *full_path,
                                   uint64_t due_ms)
{
    it_pending_event *grown;
    it_pending_event *slot;
    size_t new_cap;
    if (pending->n == pending->cap) {
        new_cap = pending->cap ? pending->cap * 2u : 8u;
        grown = (it_pending_event *)realloc(pending->v,
                                            new_cap * sizeof(*grown));
        if (!grown) return false;
        pending->v = grown;
        pending->cap = new_cap;
    }
    slot = &pending->v[pending->n];
    memset(slot, 0, sizeof(*slot));
    slot->entry_name = dup_cstr(entry_name);
    slot->full_path = dup_cstr(full_path);
    if (!slot->entry_name || !slot->full_path) {
        pending_event_free(slot);
        return false;
    }
    slot->rule_index = rule_index;
    slot->events = events;
    slot->due_ms = due_ms;
    pending->n++;
    return true;
}

static bool settle_event(it_pending_event_vec *pending, size_t rule_index,
                         it_event_mask events, const char *entry_name,
                         const char *full_path, uint32_t settle_ms)
{
    size_t item_index;
    const uint64_t due_ms = monotonic_ms() + (uint64_t)settle_ms;
    for (item_index = 0; item_index < pending->n; item_index++) {
        it_pending_event *item = &pending->v[item_index];
        if (item->rule_index == rule_index &&
            strcmp(item->full_path, full_path) == 0) {
            item->events |= events;
            item->due_ms = due_ms;
            return true;
        }
    }
    return pending_event_vec_push(pending, rule_index, events, entry_name,
                                  full_path, due_ms);
}

static int pending_event_timeout_ms(const it_pending_event_vec *pending,
                                    uint64_t now_ms)
{
    size_t item_index;
    uint64_t soonest_ms;
    if (!pending || pending->n == 0) return -1;
    soonest_ms = pending->v[0].due_ms;
    for (item_index = 1; item_index < pending->n; item_index++)
        if (pending->v[item_index].due_ms < soonest_ms)
            soonest_ms = pending->v[item_index].due_ms;
    if (soonest_ms <= now_ms) return 0;
    if (soonest_ms - now_ms > (uint64_t)INT_MAX) return INT_MAX;
    return (int)(soonest_ms - now_ms);
}

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

static bool append_quoted_arg(char **buf, size_t *len, size_t *cap,
                              const char *arg)
{
    size_t char_index;
    if (!append_bytes(buf, len, cap, "\"", 1)) return false;
    for (char_index = 0; arg[char_index] != '\0'; char_index++) {
        const char ch = arg[char_index];
        if (ch == '\\' || ch == '"') {
            char escaped[2];
            escaped[0] = '\\';
            escaped[1] = ch;
            if (!append_bytes(buf, len, cap, escaped, sizeof(escaped)))
                return false;
            continue;
        }
        if (ch == '\n') {
            if (!append_bytes(buf, len, cap, "\\n", 2)) return false;
            continue;
        }
        if (ch == '\r') {
            if (!append_bytes(buf, len, cap, "\\r", 2)) return false;
            continue;
        }
        if (ch == '\t') {
            if (!append_bytes(buf, len, cap, "\\t", 2)) return false;
            continue;
        }
        if (!append_bytes(buf, len, cap, &ch, 1)) return false;
    }
    return append_bytes(buf, len, cap, "\"", 1);
}

static char *format_exec_argv(char *const *argv)
{
    char *out = NULL;
    size_t len = 0;
    size_t cap = 0;
    size_t arg_index;
    if (!append_bytes(&out, &len, &cap, "[", 1)) return NULL;
    for (arg_index = 0; argv[arg_index] != NULL; arg_index++) {
        if (arg_index != 0 &&
            !append_bytes(&out, &len, &cap, ", ", 2)) {
            free(out);
            return NULL;
        }
        if (!append_quoted_arg(&out, &len, &cap, argv[arg_index])) {
            free(out);
            return NULL;
        }
    }
    if (!append_bytes(&out, &len, &cap, "]", 1)) {
        free(out);
        return NULL;
    }
    return out;
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

static bool install_signal_handler(int signo, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    if (sigemptyset(&sa.sa_mask) != 0) return false;
    sa.sa_flags = 0;
    return sigaction(signo, &sa, NULL) == 0;
}

/**
 * @brief Install signal handlers used for child reaping and graceful shutdown.
 *
 * @return true if all handlers were installed successfully.
 * @return false on failure.
 */
static bool install_signal_handlers(void)
{
    return install_signal_handler(SIGCHLD, on_sigchld) &&
           install_signal_handler(SIGINT, on_stop) &&
           install_signal_handler(SIGTERM, on_stop);
}

/**
 * @brief Launch a configured task process.
 *
 * Tasks always run asynchronously so the event loop can keep processing new
 * filesystem activity without blocking on child completion.
 *
 * @param rule Rule that matched and requested the task.
 * @param task Task definition to execute.
 * @param vars Event values used to expand task argument templates.
 */
static void launch_task(const it_rule *rule, const it_task *task,
                        const it_event_vars *vars)
{
    pid_t pid;
    char **argv;
    char *argv_display;
    argv = build_exec_argv(task, vars);
    if (!argv) {
        it_log_error("cannot allocate argv for task %s", task->name.s);
        return;
    }
    argv_display = format_exec_argv(argv);
    if (argv_display) {
        it_log_info("launch rule=%s task=%s event=%s path=%s argv=%s",
                    rule->name.s, task->name.s, vars->event_name,
                    vars->full_path, argv_display);
        free(argv_display);
    } else {
        it_log_warn("cannot allocate argv display for task %s", task->name.s);
        it_log_info("launch rule=%s task=%s event=%s path=%s",
                    rule->name.s, task->name.s, vars->event_name,
                    vars->full_path);
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
    it_log_info("launched rule=%s task=%s pid=%ld",
                rule->name.s, task->name.s, (long)pid);
    free_exec_argv(task, argv);
}

static void launch_rule_tasks(const it_config *cfg, const it_rule *rule,
                              const it_event_vars *vars)
{
    size_t task_index;
    for (task_index = 0; task_index < rule->run.n; task_index++) {
        const it_task *task = find_task(cfg, &rule->run.v[task_index]);
        if (!task) continue;
        launch_task(rule, task, vars);
    }
}

static void remove_pending_event(it_pending_event_vec *pending,
                                 size_t pending_index)
{
    pending_event_free(&pending->v[pending_index]);
    if (pending_index + 1u < pending->n) {
        memmove(&pending->v[pending_index], &pending->v[pending_index + 1u],
                (pending->n - pending_index - 1u) * sizeof(pending->v[0]));
    }
    pending->n--;
}

static void launch_due_settled_events(const it_config *cfg,
                                      it_pending_event_vec *pending,
                                      uint64_t now_ms)
{
    size_t pending_index = 0;
    while (pending_index < pending->n) {
        char events[64];
        it_event_vars vars;
        const it_pending_event item = pending->v[pending_index];
        const it_rule *rule;
        if (item.due_ms > now_ms) {
            pending_index++;
            continue;
        }
        rule = &cfg->rules.v[item.rule_index];
        events_to_buf(item.events, events, sizeof(events));
        vars.watch_path = rule->watch_path.s;
        vars.entry_name = item.entry_name;
        vars.full_path = item.full_path;
        vars.event_name = events;
        it_log_info("settled rule=%s event=%s path=%s",
                    rule->name.s, events, item.full_path);
        launch_rule_tasks(cfg, rule, &vars);
        remove_pending_event(pending, pending_index);
    }
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
                           it_event_mask mask, const char *name,
                           it_pending_event_vec *pending)
{
    size_t rule_index;
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
    for (rule_index = 0; rule_index < cfg->rules.n; rule_index++) {
        const it_rule *rule = &cfg->rules.v[rule_index];
        const it_event_mask matched_events = rule->events & mask;
        if (!str_eq_cstr(&rule->watch_path, target->path.s)) continue;
        if (matched_events == 0) continue;
        if (!rule_name_filter_matches(rule, vars.entry_name)) continue;
        any = true;
        it_log_info("rule %s matched", rule->name.s);
        if (rule->settle_ms != IT_RULE_SETTLE_MS_DEFAULT) {
            if (settle_event(pending, rule_index, matched_events,
                             vars.entry_name, vars.full_path,
                             rule->settle_ms)) {
                it_log_info("settling rule=%s path=%s quiet_ms=%u",
                            rule->name.s, vars.full_path,
                            (unsigned)rule->settle_ms);
            } else {
                it_log_error("cannot allocate settled event for rule=%s path=%s",
                             rule->name.s, vars.full_path);
            }
        } else {
            launch_rule_tasks(cfg, rule, &vars);
        }
    }
    if (!any) it_log_info("no rule matched");
    free(full_path);
}

static void print_usage(const char *program)
{
    (void)fprintf(stderr, "usage: %s [--check] <config-file>\n", program);
}

static void warn_check_findings(const it_config *cfg)
{
    size_t rule_index;
    for (rule_index = 0; rule_index < cfg->rules.n; rule_index++) {
        const it_rule *rule = &cfg->rules.v[rule_index];
        if ((rule->events & IT_EVT_MODIFY) != 0 &&
            rule->settle_ms == IT_RULE_SETTLE_MS_DEFAULT) {
            it_log_warn("rule %s watches MODIFY with settle_ms=0; active writes may launch repeated tasks",
                        rule->name.s);
        }
    }
}

int main(int argc, char **argv)
{
    it_config cfg;
    it_runtime_plan plan;
    it_runtime_session session;
    it_pending_event_vec pending;
    const char *config_path;
    bool check_only = false;
    char buf[4096];
    it_log_set_level(IT_LOG_INFO);
    if (argc == 2) {
        config_path = argv[1];
    } else if (argc == 3 && strcmp(argv[1], "--check") == 0) {
        check_only = true;
        config_path = argv[2];
    } else {
        print_usage(argv[0]);
        return 2;
    }
    if (!it_load_config_file(config_path, &cfg)) return 1;
    it_runtime_plan_init(&plan);
    it_runtime_session_init(&session);
    pending_event_vec_init(&pending);
    if (!it_runtime_plan_build(&cfg, &plan)) {
        it_log_error("cannot build runtime watch plan");
        pending_event_vec_free(&pending);
        it_config_free(&cfg);
        return 1;
    }
    print_config_summary(&cfg, &plan);
    if (check_only) {
        warn_check_findings(&cfg);
        printf("\nConfig check passed; runtime watches were not opened.\n");
        pending_event_vec_free(&pending);
        it_runtime_plan_free(&plan);
        it_config_free(&cfg);
        return 0;
    }
    if (!it_runtime_session_open(&cfg, &plan, &session)) {
        it_log_error("cannot open inotify watches");
        pending_event_vec_free(&pending);
        it_runtime_plan_free(&plan);
        it_config_free(&cfg);
        return 1;
    }
    if (!install_signal_handlers()) {
        it_log_error("cannot install signal handlers: %s", strerror(errno));
        pending_event_vec_free(&pending);
        it_runtime_session_free(&session);
        it_runtime_plan_free(&plan);
        it_config_free(&cfg);
        return 1;
    }
    it_log_info("watching for filesystem events; press Ctrl-C to stop");
    while (!g_stop_requested) {
        struct pollfd watch_poll;
        int poll_timeout;
        int poll_result;
        uint64_t now_ms;
        if (g_reap_requested) reap_children();
        now_ms = monotonic_ms();
        launch_due_settled_events(&cfg, &pending, now_ms);
        poll_timeout = pending_event_timeout_ms(&pending, monotonic_ms());
        watch_poll.fd = session.fd;
        watch_poll.events = POLLIN;
        watch_poll.revents = 0;
        poll_result = poll(&watch_poll, 1, poll_timeout);
        if (poll_result < 0) {
            if (errno == EINTR) {
                if (g_reap_requested) reap_children();
                if (g_stop_requested) break;
                continue;
            }
            it_log_error("poll failed: %s", strerror(errno));
            break;
        }
        if (poll_result == 0) {
            continue;
        }
        if ((watch_poll.revents & POLLIN) != 0) {
            ssize_t nread;
            size_t off = 0;
            nread = read(session.fd, buf, sizeof(buf));
            if (nread < 0) {
                if (errno == EINTR) {
                    if (g_reap_requested) reap_children();
                    if (g_stop_requested) break;
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
                if ((ev->mask & IN_Q_OVERFLOW) != 0) {
                    it_log_error("inotify queue overflow; filesystem events may have been lost");
                    off += ev_size;
                    continue;
                }
                target = it_runtime_session_target_for_wd(&plan, &session, ev->wd);
                if (!target) {
                    off += ev_size;
                    continue;
                }
                mask = it_runtime_event_mask_from_inotify(ev->mask);
                if (mask != 0)
                    dispatch_event(&cfg, target, mask, ev->len ? ev->name : "",
                                   &pending);
                off += ev_size;
            }
        }
        if ((watch_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            it_log_error("inotify poll reported revents=0x%x",
                         watch_poll.revents);
            break;
        }
        if (g_reap_requested) reap_children();
    }
    if (g_stop_requested) it_log_info("shutdown requested; exiting event loop");
    reap_children();
    pending_event_vec_free(&pending);
    it_runtime_session_free(&session);
    it_runtime_plan_free(&plan);
    it_config_free(&cfg);
    return 0;
}
