/*
 * Copyright (c) 2026 Adam Young
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INOTASK_LEXER_H
#define INOTASK_LEXER_H

#include <stddef.h>

/**
 * @brief Token kinds produced by the configuration lexer.
 */
typedef enum it_tok_kind {
    IT_TOK_EOF = 0,
    IT_TOK_ERROR,
    IT_TOK_IDENT,
    IT_TOK_STRING,
    IT_TOK_LBRACE,
    IT_TOK_RBRACE,
    IT_TOK_LBRACKET,
    IT_TOK_RBRACKET,
    IT_TOK_COMMA,
    IT_TOK_EQUALS
} it_tok_kind;

/**
 * @brief Token emitted by the lexer.
 */
typedef struct it_token {
    it_tok_kind kind;
    const char *text;
    size_t len;
    size_t line;
    size_t col;
} it_token;

/**
 * @brief Stateful lexer over a configuration input buffer.
 */
typedef struct it_lexer {
    const char *buf;
    size_t len;
    size_t pos;
    size_t line;
    size_t col;
} it_lexer;

/**
 * @brief Initialize a lexer over a configuration buffer.
 *
 * @param lx Lexer instance to initialize.
 * @param buf Input buffer to tokenize.
 * @param len Number of bytes in @p buf.
 */
void it_lexer_init(it_lexer *lx, const char *buf, size_t len);

/**
 * @brief Read the next token from the input stream.
 *
 * @param lx Lexer instance.
 *
 * @return Next token from the input buffer.
 */
it_token it_lexer_next(it_lexer *lx);

/**
 * @brief Convert a token kind to a readable string.
 *
 * @param kind Token kind to describe.
 *
 * @return Constant string describing the token kind.
 */
const char *it_tok_kind_str(it_tok_kind kind);

#endif
