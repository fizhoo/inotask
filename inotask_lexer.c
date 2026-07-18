/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#include "inotask_lexer.h"

#include <ctype.h>
#include <stdio.h>

static it_token token(it_tok_kind kind, const char *text, size_t len,
                      size_t line, size_t col)
{
    it_token t;
    t.kind = kind;
    t.text = text;
    t.len = len;
    t.line = line;
    t.col = col;
    return t;
}

void it_lexer_init(it_lexer *lx, const char *buf, size_t len)
{
    lx->buf = buf;
    lx->len = len;
    lx->pos = 0;
    lx->line = 1;
    lx->col = 1;
}

static int peek(const it_lexer *lx)
{
    return lx->pos < lx->len ? (unsigned char)lx->buf[lx->pos] : EOF;
}

static int take(it_lexer *lx)
{
    int c = peek(lx);
    if (c == EOF) return EOF;
    lx->pos++;
    if (c == '\n') {
        lx->line++;
        lx->col = 1;
    } else {
        lx->col++;
    }
    return c;
}

static void skip_space_and_comments(it_lexer *lx)
{
    for (;;) {
        int c = peek(lx);
        while (c != EOF && isspace((unsigned char)c)) {
            (void)take(lx);
            c = peek(lx);
        }
        if (c == '#') {
            while (c != EOF && c != '\n') {
                (void)take(lx);
                c = peek(lx);
            }
            continue;
        }
        break;
    }
}

it_token it_lexer_next(it_lexer *lx)
{
    size_t start, line, col;
    int c;

    skip_space_and_comments(lx);
    start = lx->pos;
    line = lx->line;
    col = lx->col;
    c = peek(lx);

    if (c == EOF) return token(IT_TOK_EOF, lx->buf + lx->pos, 0, line, col);

    switch (c) {
        case '{': (void)take(lx); return token(IT_TOK_LBRACE, lx->buf + start, 1, line, col);
        case '}': (void)take(lx); return token(IT_TOK_RBRACE, lx->buf + start, 1, line, col);
        case '[': (void)take(lx); return token(IT_TOK_LBRACKET, lx->buf + start, 1, line, col);
        case ']': (void)take(lx); return token(IT_TOK_RBRACKET, lx->buf + start, 1, line, col);
        case ',': (void)take(lx); return token(IT_TOK_COMMA, lx->buf + start, 1, line, col);
        case '=': (void)take(lx); return token(IT_TOK_EQUALS, lx->buf + start, 1, line, col);
        case '"': {
            (void)take(lx);
            start = lx->pos;
            while ((c = peek(lx)) != EOF && c != '"') {
                if (c == '\n' || c == '\r' || c == '\\')
                    return token(IT_TOK_ERROR, lx->buf + start, lx->pos - start, line, col);
                (void)take(lx);
            }
            if (c != '"')
                return token(IT_TOK_ERROR, lx->buf + start, lx->pos - start, line, col);
            {
                size_t n = lx->pos - start;
                (void)take(lx);
                return token(IT_TOK_STRING, lx->buf + start, n, line, col);
            }
        }
        default:
            break;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        (void)take(lx);
        while ((c = peek(lx)) != EOF &&
               (isalnum((unsigned char)c) || c == '_' || c == '-'))
            (void)take(lx);
        return token(IT_TOK_IDENT, lx->buf + start, lx->pos - start, line, col);
    }

    (void)take(lx);
    return token(IT_TOK_ERROR, lx->buf + start, 1, line, col);
}

const char *it_tok_kind_str(it_tok_kind kind)
{
    switch (kind) {
        case IT_TOK_EOF: return "end of file";
        case IT_TOK_ERROR: return "invalid token";
        case IT_TOK_IDENT: return "identifier";
        case IT_TOK_STRING: return "string";
        case IT_TOK_LBRACE: return "{";
        case IT_TOK_RBRACE: return "}";
        case IT_TOK_LBRACKET: return "[";
        case IT_TOK_RBRACKET: return "]";
        case IT_TOK_COMMA: return ",";
        case IT_TOK_EQUALS: return "=";
        default: return "unknown token";
    }
}
