/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#include "inotask_parser.h"
#include "inotask_lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct parser {
    it_lexer lx;
    it_token cur;
    it_config *cfg;
    it_parse_error *err;
} parser;

/**
 * @brief Duplicate an arbitrary byte range as a NUL-terminated string.
 *
 * This is used for token slices that are not NUL-terminated in the source
 * buffer.
 *
 * @param s Source byte range.
 * @param n Number of bytes to copy.
 *
 * @return Newly allocated NUL-terminated string, or NULL on allocation
 *         failure.
 */
static char *dup_n(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/**
 * @brief Record the first parse error encountered.
 *
 * The parser preserves the earliest failure location and ignores later
 * attempts to overwrite the error detail.
 *
 * @param p Parser state.
 * @param fmt `printf`-style format string used to build the detail.
 * @param arg Optional single substitution argument.
 */
static void set_error(parser *p, const char *fmt, const char *arg)
{
    char buf[256];
    size_t len;
    if (!p->err || p->err->detail) return;
    (void)snprintf(buf, sizeof(buf), fmt, arg ? arg : "");
    p->err->line = p->cur.line;
    p->err->col = p->cur.col;
    len = strlen(buf);
    p->err->detail = (char *)malloc(len + 1);
    if (!p->err->detail) return;
    memcpy(p->err->detail, buf, len + 1);
}

static bool fail(parser *p, const char *fmt, const char *arg)
{
    set_error(p, fmt, arg);
    return false;
}

/**
 * @brief Consume the current token and read the next one from the lexer.
 *
 * @param p Parser state.
 */
static void advance(parser *p) { p->cur = it_lexer_next(&p->lx); }

/**
 * @brief Verify the current token kind and consume it.
 *
 * @param p Parser state.
 * @param kind Expected token kind.
 * @param what Human-readable token description used in diagnostics.
 *
 * @return true if the expected token was present.
 * @return false if the current token does not match.
 */
static bool expect(parser *p, it_tok_kind kind, const char *what)
{
    if (p->cur.kind != kind) {
        char buf[128];
        (void)snprintf(buf, sizeof(buf), "expected %s, got %s",
                       what, it_tok_kind_str(p->cur.kind));
        return fail(p, "%s", buf);
    }
    advance(p);
    return true;
}

static bool token_is(const it_token *t, const char *s)
{
    size_t n = strlen(s);
    return t->len == n && memcmp(t->text, s, n) == 0;
}

static bool parse_string(parser *p, char **out)
{
    if (p->cur.kind != IT_TOK_STRING) return fail(p, "expected string literal%s", "");
    *out = dup_n(p->cur.text, p->cur.len);
    if (!*out) return fail(p, "out of memory%s", "");
    advance(p);
    return true;
}

static bool parse_ident(parser *p, char **out)
{
    if (p->cur.kind != IT_TOK_IDENT) return fail(p, "expected identifier%s", "");
    *out = dup_n(p->cur.text, p->cur.len);
    if (!*out) return fail(p, "out of memory%s", "");
    advance(p);
    return true;
}

/**
 * @brief Convert a parsed event identifier token into an event-mask bit.
 *
 * @param t Identifier token to interpret.
 * @param bit Output event bit when the identifier is recognized.
 *
 * @return true if the identifier names a known event.
 * @return false otherwise.
 */
static bool event_bit(const it_token *t, it_event_mask *bit)
{
    if (token_is(t, "CREATE")) *bit = IT_EVT_CREATE;
    else if (token_is(t, "MODIFY")) *bit = IT_EVT_MODIFY;
    else if (token_is(t, "DELETE")) *bit = IT_EVT_DELETE;
    else if (token_is(t, "MOVE")) *bit = IT_EVT_MOVE;
    else if (token_is(t, "ATTRIB")) *bit = IT_EVT_ATTRIB;
    else if (token_is(t, "CLOSE_WRITE")) *bit = IT_EVT_CLOSE_WRITE;
    else return false;
    return true;
}

/**
 * @brief Parse an event list such as `[ CREATE, MODIFY ]`.
 *
 * This helper rejects empty lists, duplicate event names, and unknown event
 * identifiers.
 *
 * @param p Parser state.
 * @param out Output event mask.
 *
 * @return true if the list was parsed successfully.
 * @return false on syntax error, semantic error, or allocation failure.
 */
static bool parse_events(parser *p, it_event_mask *out)
{
    it_event_mask mask = 0, bit;
    char *name;
    if (!expect(p, IT_TOK_LBRACKET, "'['")) return false;
    if (p->cur.kind == IT_TOK_RBRACKET) return fail(p, "events list must not be empty%s", "");
    for (;;) {
        if (p->cur.kind != IT_TOK_IDENT) return fail(p, "expected event identifier%s", "");
        if (!event_bit(&p->cur, &bit)) {
            name = dup_n(p->cur.text, p->cur.len);
            if (!name) return fail(p, "out of memory%s", "");
            set_error(p, "unknown event '%s'", name);
            free(name);
            return false;
        }
        if ((mask & bit) != 0) {
            name = dup_n(p->cur.text, p->cur.len);
            if (!name) return fail(p, "out of memory%s", "");
            set_error(p, "duplicate event '%s'", name);
            free(name);
            return false;
        }
        mask |= bit;
        advance(p);
        if (p->cur.kind != IT_TOK_COMMA) break;
        advance(p);
        if (p->cur.kind == IT_TOK_RBRACKET)
            return fail(p, "trailing comma in events list%s", "");
    }
    if (!expect(p, IT_TOK_RBRACKET, "']'")) return false;
    *out = mask;
    return true;
}

static void free_strings(char **v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) free(v[i]);
    free(v);
}

/**
 * @brief Parse a list of string literals.
 *
 * The resulting array owns each copied string and must be released with
 * `free_strings()` by the caller.
 *
 * @param p Parser state.
 * @param out_v Output array of heap-allocated strings.
 * @param out_n Output array length.
 * @param name Human-readable field name used in diagnostics.
 *
 * @return true if the list was parsed successfully.
 * @return false on syntax error, semantic error, or allocation failure.
 */
static bool parse_string_list(parser *p, char ***out_v, size_t *out_n,
                              const char *name)
{
    char **v = NULL, **nv;
    size_t n = 0, cap = 0, nc;
    char *s;
    if (!expect(p, IT_TOK_LBRACKET, "'['")) return false;
    if (p->cur.kind == IT_TOK_RBRACKET) {
        char buf[128];
        (void)snprintf(buf, sizeof(buf), "%s list must not be empty", name);
        return fail(p, "%s", buf);
    }
    for (;;) {
        if (!parse_string(p, &s)) { free_strings(v, n); return false; }
        if (n == cap) {
            nc = cap ? cap * 2 : 8;
            nv = (char **)realloc(v, nc * sizeof(*nv));
            if (!nv) { free(s); free_strings(v, n); return fail(p, "out of memory%s", ""); }
            v = nv; cap = nc;
        }
        v[n++] = s;
        if (p->cur.kind != IT_TOK_COMMA) break;
        advance(p);
        if (p->cur.kind == IT_TOK_RBRACKET) {
            free_strings(v, n);
            return fail(p, "trailing comma in string list%s", "");
        }
    }
    if (!expect(p, IT_TOK_RBRACKET, "']'")) { free_strings(v, n); return false; }
    *out_v = v; *out_n = n;
    return true;
}

static bool add_result(parser *p, it_cfg_errc rc)
{
    if (rc == IT_CFG_OK) return true;
    return fail(p, "%s", it_cfg_errc_str(rc));
}

static bool parse_task(parser *p)
{
    char *name = NULL, *field = NULL, *exec = NULL;
    char **args = NULL;
    size_t args_n = 0;
    bool have_exec = false, have_args = false;
    advance(p);
    if (!parse_ident(p, &name)) return false;
    if (!expect(p, IT_TOK_LBRACE, "'{'")) { free(name); return false; }
    while (p->cur.kind != IT_TOK_RBRACE) {
        if (p->cur.kind == IT_TOK_EOF) { free(name); free(exec); free_strings(args, args_n); return fail(p, "unexpected end of task block%s", ""); }
        if (!parse_ident(p, &field)) { free(name); free(exec); free_strings(args, args_n); return false; }
        if (!expect(p, IT_TOK_EQUALS, "'='")) { free(field); free(name); free(exec); free_strings(args, args_n); return false; }
        if (strcmp(field, "exec") == 0) {
            free(field); field = NULL;
            if (have_exec) { free(name); free(exec); free_strings(args, args_n); return fail(p, "duplicate task field 'exec'%s", ""); }
            if (!parse_string(p, &exec)) { free(name); free_strings(args, args_n); return false; }
            have_exec = true;
        } else if (strcmp(field, "args") == 0) {
            free(field); field = NULL;
            if (have_args) { free(name); free(exec); free_strings(args, args_n); return fail(p, "duplicate task field 'args'%s", ""); }
            if (!parse_string_list(p, &args, &args_n, "args")) { free(name); free(exec); return false; }
            have_args = true;
        } else {
            set_error(p, "unknown task field '%s'", field);
            free(field); free(name); free(exec); free_strings(args, args_n); return false;
        }
    }
    advance(p);
    if (!have_exec) { free(name); free_strings(args, args_n); return fail(p, "task missing required field 'exec'%s", ""); }
    {
        it_cfg_errc rc;
        rc = it_config_add_task(p->cfg, name, exec, (const char *const *)args,
                                args_n);
        free(name); free(exec); free_strings(args, args_n);
        return add_result(p, rc);
    }
}

static bool parse_rule(parser *p)
{
    char *name = NULL, *field = NULL, *watch = NULL;
    char **include = NULL, **exclude = NULL;
    char **run = NULL;
    size_t include_n = 0, exclude_n = 0, run_n = 0;
    bool have_watch = false, have_events = false, have_include = false,
         have_exclude = false, have_run = false;
    it_event_mask events = 0;
    advance(p);
    if (!parse_ident(p, &name)) return false;
    if (!expect(p, IT_TOK_LBRACE, "'{'")) { free(name); return false; }
    while (p->cur.kind != IT_TOK_RBRACE) {
        if (p->cur.kind == IT_TOK_EOF) { fail(p, "unexpected end of rule block%s", ""); goto bad; }
        if (!parse_ident(p, &field)) goto bad;
        if (!expect(p, IT_TOK_EQUALS, "'='")) { free(field); field = NULL; goto bad; }
        if (strcmp(field, "watch") == 0) {
            free(field); field = NULL;
            if (have_watch) { fail(p, "duplicate rule field 'watch'%s", ""); goto bad; }
            if (!parse_string(p, &watch)) goto bad;
            have_watch = true;
        } else if (strcmp(field, "events") == 0) {
            free(field); field = NULL;
            if (have_events) { fail(p, "duplicate rule field 'events'%s", ""); goto bad; }
            if (!parse_events(p, &events)) goto bad;
            have_events = true;
        } else if (strcmp(field, "run") == 0) {
            free(field); field = NULL;
            if (have_run) { fail(p, "duplicate rule field 'run'%s", ""); goto bad; }
            if (!parse_string_list(p, &run, &run_n, "run")) goto bad;
            have_run = true;
        } else if (strcmp(field, "include") == 0) {
            free(field); field = NULL;
            if (have_include) { fail(p, "duplicate rule field 'include'%s", ""); goto bad; }
            if (!parse_string_list(p, &include, &include_n, "include")) goto bad;
            have_include = true;
        } else if (strcmp(field, "exclude") == 0) {
            free(field); field = NULL;
            if (have_exclude) { fail(p, "duplicate rule field 'exclude'%s", ""); goto bad; }
            if (!parse_string_list(p, &exclude, &exclude_n, "exclude")) goto bad;
            have_exclude = true;
        } else {
            set_error(p, "unknown rule field '%s'", field);
            free(field); field = NULL; goto bad;
        }
    }
    advance(p);
    if (!have_watch) { fail(p, "rule missing required field 'watch'%s", ""); goto bad; }
    if (!have_events) { fail(p, "rule missing required field 'events'%s", ""); goto bad; }
    if (!have_run) { fail(p, "rule missing required field 'run'%s", ""); goto bad; }
    {
        it_cfg_errc rc = it_config_add_rule(p->cfg, name, watch, events,
                                             (const char *const *)include, include_n,
                                             (const char *const *)exclude, exclude_n,
                                             (const char *const *)run, run_n);
        free(name); free(watch);
        free_strings(include, include_n); free_strings(exclude, exclude_n);
        free_strings(run, run_n);
        return add_result(p, rc);
    }
bad:
    free(name); free(watch); free(field);
    free_strings(include, include_n); free_strings(exclude, exclude_n);
    free_strings(run, run_n);
    return false;
}

bool it_parse_config(const char *buf, size_t len, it_config *cfg,
                     it_parse_error *err)
{
    parser p;
    memset(&p, 0, sizeof(p));
    p.cfg = cfg; p.err = err;
    if (err) { err->line = 0; err->col = 0; err->detail = NULL; }
    it_lexer_init(&p.lx, buf, len);
    advance(&p);
    while (p.cur.kind != IT_TOK_EOF) {
        if (p.cur.kind == IT_TOK_ERROR) return fail(&p, "lexical error%s", "");
        if (p.cur.kind != IT_TOK_IDENT) return fail(&p, "expected task or rule%s", "");
        if (token_is(&p.cur, "task")) { if (!parse_task(&p)) return false; }
        else if (token_is(&p.cur, "rule")) { if (!parse_rule(&p)) return false; }
        else return fail(&p, "unknown top-level keyword%s", "");
    }
    return true;
}

void it_parse_error_free(it_parse_error *err)
{
    if (!err) return;
    free(err->detail);
    err->detail = NULL;
}
