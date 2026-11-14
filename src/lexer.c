#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "acsh.h"
#include "lex.h"

/* The lexer turns input text into tokens. Words are decoded into the
 * marked form expansion expects:
 *
 *   'text', \c and escaped characters inside "..."  ->  SQ_START text SQ_END
 *   \$ inside "..."                                 ->  ESCAPED_DOLLAR
 *   $(...), `...`, ${...}                            ->  copied verbatim
 *   first character quoted                         ->  QUOTED_FIRST prefix
 *
 * When a quote, substitution or construct is still open at the end of
 * the buffer, the lexer asks its LineSource for another line; that is
 * how one command can span several lines. */

#define END (-1)

/* ---- input buffer ---- */

static void append(Lexer *lx, const char *s) {
    size_t n = strlen(s);
    if (lx->len + n + 1 > lx->cap) {
        size_t cap = lx->cap ? lx->cap : 256;
        while (lx->len + n + 1 > cap) cap *= 2;
        char *bigger = realloc(lx->buf, cap);
        if (bigger == NULL) {
            fprintf(stderr, "acsh: out of memory\n");
            exit(2);
        }
        lx->buf = bigger;
        lx->cap = cap;
    }
    memcpy(lx->buf + lx->len, s, n + 1);
    lx->len += n;
}

/* Fetches one more line from the source. Returns 0 at end of input. */
static int fill(Lexer *lx) {
    if (lx->source == NULL || lx->at_eof) {
        return 0;
    }
    char *line = lx->source(lx->ctx, !lx->new_command);
    lx->new_command = 0;
    if (line == NULL) {
        lx->at_eof = 1;
        return 0;
    }
    append(lx, line);
    free(line);
    return 1;
}

/* The character `off` places ahead, reading more input if needed. */
static int ch(Lexer *lx, size_t off) {
    while (lx->pos + off >= lx->len) {
        if (!fill(lx)) {
            return END;
        }
    }
    return (unsigned char)lx->buf[lx->pos + off];
}

Lexer *lexer_new(const char *text, LineSource source, void *ctx, const char *name) {
    Lexer *lx = calloc(1, sizeof(Lexer));
    if (lx == NULL) {
        fprintf(stderr, "acsh: out of memory\n");
        exit(2);
    }
    append(lx, text != NULL ? text : "");
    lx->source = source;
    lx->ctx = ctx;
    lx->new_command = 1;
    lx->name = name != NULL ? strdup(name) : NULL;
    return lx;
}

void lexer_free(Lexer *lx) {
    if (lx != NULL) {
        free(lx->buf);
        free(lx->name);
        free(lx);
    }
}

void lex_begin_command(Lexer *lx) {
    for (size_t i = 0; i < lx->pos; i++) {
        if (lx->buf[i] == '\n') lx->base_line++;
    }
    memmove(lx->buf, lx->buf + lx->pos, lx->len - lx->pos + 1);
    lx->len -= lx->pos;
    lx->pos = 0;
    lx->alias_end = 0;
    lx->new_command = 1;
}

void lex_discard(Lexer *lx) {
    lx->pos = lx->len;
}

void lex_discard_input(Lexer *lx) {
    lex_discard(lx);
}

char *lex_source(Lexer *lx, size_t start, size_t end) {
    return strndup(lx->buf + start, end - start);
}

void lex_syntax_error(Lexer *lx, size_t pos, const char *usage, const char *fmt, ...) {
    va_list ap;
    if (lx->name != NULL) {
        int line = lx->base_line + 1;
        for (size_t i = 0; i < pos && i < lx->len; i++) {
            if (lx->buf[i] == '\n') line++;
        }
        fprintf(stderr, "acsh: %s: line %d: syntax error: ", lx->name, line);
    } else {
        fprintf(stderr, "acsh: syntax error: ");
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (usage != NULL) {
        fprintf(stderr, "  acsh: usage:  %s\n", usage);
    }
}

/* ---- words ---- */

typedef struct {
    char  *s;
    size_t len, cap;
} Str;

static void put(Str *b, char c) {
    if (b->len + 2 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 32;
        b->s = realloc(b->s, b->cap);
        if (b->s == NULL) {
            fprintf(stderr, "acsh: out of memory\n");
            exit(2);
        }
    }
    b->s[b->len++] = c;
    b->s[b->len] = '\0';
}

/* A character escaped by a backslash or quotes: literal, never expanded
 * or globbed. */
static void put_literal(Str *b, char c) {
    put(b, SQ_START);
    put(b, c);
    put(b, SQ_END);
}

static int unterminated(Lexer *lx, size_t start, const char *what, const char *hint) {
    lex_syntax_error(lx, start, NULL, "%s", what);
    fprintf(stderr, "  acsh: %s\n", hint);
    return -1;
}

/* Copies a $( ... ) region verbatim; nested parentheses and quotes are
 * tracked so the right ')' ends it. */
static int copy_paren_subst(Lexer *lx, Str *out, size_t start) {
    put(out, (char)ch(lx, 0)); /* $ */
    put(out, (char)ch(lx, 1)); /* ( */
    lx->pos += 2;
    int depth = 1;
    int quote = 0;
    for (;;) {
        int c = ch(lx, 0);
        if (c == END) {
            return unterminated(lx, start, "no closing ')' found for '$('",
                                "command substitution needs a matching ')': $(command)");
        }
        lx->pos++;
        put(out, (char)c);
        if (quote) {
            if (c == '\\' && quote == '"' && ch(lx, 0) != END) {
                put(out, (char)ch(lx, 0));
                lx->pos++;
            } else if (c == quote) {
                quote = 0;
            }
        } else if (c == '\\' && ch(lx, 0) != END) {
            put(out, (char)ch(lx, 0));
            lx->pos++;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '(') {
            depth++;
        } else if (c == ')' && --depth == 0) {
            return 0;
        }
    }
}

/* Copies a ${ ... } region verbatim. */
static int copy_brace_param(Lexer *lx, Str *out, size_t start) {
    put(out, '$');
    put(out, '{');
    lx->pos += 2;
    int depth = 1;
    for (;;) {
        int c = ch(lx, 0);
        if (c == END) {
            return unterminated(lx, start, "no closing '}' found for '${'",
                                "parameter expansion needs a matching '}': ${NAME}");
        }
        lx->pos++;
        put(out, (char)c);
        if (c == '\\' && ch(lx, 0) != END) {
            put(out, (char)ch(lx, 0));
            lx->pos++;
        } else if (c == '{') {
            depth++;
        } else if (c == '}' && --depth == 0) {
            return 0;
        }
    }
}

/* Copies a `...` region verbatim. */
static int copy_backticks(Lexer *lx, Str *out, size_t start) {
    put(out, '`');
    lx->pos++;
    for (;;) {
        int c = ch(lx, 0);
        if (c == END) {
            return unterminated(lx, start, "no closing '`' found",
                                "backtick command substitution needs a matching closing `");
        }
        lx->pos++;
        put(out, (char)c);
        if (c == '\\' && ch(lx, 0) != END) {
            put(out, (char)ch(lx, 0));
            lx->pos++;
        } else if (c == '`') {
            return 0;
        }
    }
}

static int is_word_end(int c) {
    return c == END || c == ' ' || c == '\t' || c == '\n' ||
           (c != '\0' && strchr(";&|<>()", c) != NULL);
}

static int scan_word(Lexer *lx, Token *t) {
    Str out = { NULL, 0, 0 };
    int first = 1;
    int first_quoted = 0;
    put(&out, QUOTED_FIRST); /* placeholder, removed below if unused */
    t->type = T_WORD;
    t->quoted = 0;

    for (;;) {
        int c = ch(lx, 0);
        if (is_word_end(c)) {
            break;
        }
        size_t here = lx->pos;
        if (c == '\\') {
            int d = ch(lx, 1);
            if (d == '\n') {           /* line continuation */
                lx->pos += 2;
                continue;
            }
            if (d == END) {            /* lone backslash at the very end */
                put(&out, '\\');
                lx->pos++;
                first = 0;
                continue;
            }
            if (first) first_quoted = 1;
            t->quoted = 1;
            put_literal(&out, (char)d);
            lx->pos += 2;
        } else if (c == '\'') {
            if (first) first_quoted = 1;
            t->quoted = 1;
            lx->pos++;
            put(&out, SQ_START);
            for (;;) {
                int d = ch(lx, 0);
                if (d == END) {
                    free(out.s);
                    return unterminated(lx, here, "no closing ' found",
                                        "every ' must be matched by another ' later on");
                }
                lx->pos++;
                if (d == '\'') break;
                put(&out, (char)d);
            }
            put(&out, SQ_END);
        } else if (c == '"') {
            if (first) first_quoted = 1;
            t->quoted = 1;
            lx->pos++;
            for (;;) {
                int d = ch(lx, 0);
                if (d == END) {
                    free(out.s);
                    return unterminated(lx, here, "no closing \" found",
                                        "every \" must be matched by another \" later on");
                }
                if (d == '"') {
                    lx->pos++;
                    break;
                }
                if (d == '\\') {
                    int e = ch(lx, 1);
                    if (e == '\n') {
                        lx->pos += 2;
                    } else if (e == '$') {
                        put(&out, ESCAPED_DOLLAR);
                        lx->pos += 2;
                    } else if (e == '"' || e == '\\' || e == '`') {
                        put_literal(&out, (char)e);
                        lx->pos += 2;
                    } else {
                        put(&out, '\\');
                        lx->pos++;
                    }
                } else if (d == '$' && ch(lx, 1) == '(') {
                    if (copy_paren_subst(lx, &out, lx->pos) != 0) { free(out.s); return -1; }
                } else if (d == '$' && ch(lx, 1) == '{') {
                    if (copy_brace_param(lx, &out, lx->pos) != 0) { free(out.s); return -1; }
                } else if (d == '`') {
                    if (copy_backticks(lx, &out, lx->pos) != 0) { free(out.s); return -1; }
                } else if (d == '$' && ch(lx, 1) != END && strchr("?*@#!$-0123456789", ch(lx, 1)) != NULL) {
                    put(&out, '$');               /* $?, $*, $1 ...: the next */
                    put(&out, (char)ch(lx, 1));   /* character belongs to the $ */
                    lx->pos += 2;
                } else if (strchr("*?[ \t\n", d) != NULL) {
                    put_literal(&out, (char)d); /* no globbing or splitting inside "..." */
                    lx->pos++;
                } else {
                    put(&out, (char)d);
                    lx->pos++;
                }
            }
        } else if (c == '$' && ch(lx, 1) == '(') {
            if (copy_paren_subst(lx, &out, here) != 0) { free(out.s); return -1; }
        } else if (c == '$' && ch(lx, 1) == '{') {
            if (copy_brace_param(lx, &out, here) != 0) { free(out.s); return -1; }
        } else if (c == '`') {
            if (copy_backticks(lx, &out, here) != 0) { free(out.s); return -1; }
        } else {
            put(&out, (char)c);
            lx->pos++;
        }
        first = 0;
    }

    if (!first_quoted) {
        memmove(out.s, out.s + 1, out.len); /* drop the QUOTED_FIRST placeholder */
    }
    t->text = out.s;
    return 0;
}

/* ---- tokens ---- */

int lex_next(Lexer *lx, Token *t) {
    t->text = NULL;
    t->quoted = 0;

    /* blanks, line continuations and comments */
    for (;;) {
        int c = ch(lx, 0);
        if (c == ' ' || c == '\t') {
            lx->pos++;
        } else if (c == '\\' && ch(lx, 1) == '\n') {
            lx->pos += 2;
        } else if (c == '#') {
            while (ch(lx, 0) != END && ch(lx, 0) != '\n') lx->pos++;
        } else {
            break;
        }
    }

    t->start = lx->pos;
    int c = ch(lx, 0);
    if (c == END) {
        t->type = T_EOF;
        t->end = lx->pos;
        return 0;
    }

    /* an IO_NUMBER is a run of digits directly followed by < or > */
    if (isdigit(c)) {
        size_t n = 0;
        while (isdigit(ch(lx, n))) n++;
        int after = ch(lx, n);
        if (after == '<' || after == '>') {
            t->type = T_IO_NUMBER;
            t->text = strndup(lx->buf + lx->pos, n);
            lx->pos += n;
            t->end = lx->pos;
            return 0;
        }
    }

    /* Look ahead only for operator characters, and only as far as the
     * operator can reach: reading past a final newline would ask the
     * source for another line and print a stray PS2 prompt. */
    int is_op = (c != '\n' && strchr(";&|()<>", c) != NULL);
    int d = is_op ? ch(lx, 1) : END;
    int e = (c == '<' && d == '<') ? ch(lx, 2) : END;
    int len = 1;
    switch (c) {
    case '\n': t->type = T_NEWLINE; break;
    case ';':  if (d == ';') { t->type = T_DSEMI;  len = 2; } else t->type = T_SEMI; break;
    case '&':  if (d == '&') { t->type = T_AND_IF; len = 2; } else t->type = T_AMP; break;
    case '|':  if (d == '|') { t->type = T_OR_IF;  len = 2; } else t->type = T_PIPE; break;
    case '(':  t->type = T_LPAREN; break;
    case ')':  t->type = T_RPAREN; break;
    case '<':
        if (d == '<' && e == '-') { t->type = T_DLESSDASH; len = 3; }
        else if (d == '<') { t->type = T_DLESS; len = 2; }
        else if (d == '&') { t->type = T_LESSAND; len = 2; }
        else if (d == '>') { t->type = T_LESSGREAT; len = 2; }
        else t->type = T_LESS;
        break;
    case '>':
        if (d == '>') { t->type = T_DGREAT; len = 2; }
        else if (d == '&') { t->type = T_GREATAND; len = 2; }
        else if (d == '|') { t->type = T_CLOBBER; len = 2; }
        else t->type = T_GREAT;
        break;
    default:
        if (scan_word(lx, t) != 0) {
            return -1;
        }
        t->end = lx->pos;
        return 0;
    }
    lx->pos += (size_t)len;
    t->end = lx->pos;
    return 0;
}

int lex_expand_alias(Lexer *lx, const Token *t) {
    if (t->type != T_WORD || t->quoted || t->start < lx->alias_end) {
        return 0;
    }
    char *name = lex_source(lx, t->start, t->end);
    const char *value = (name != NULL) ? alias_get(name) : NULL;
    free(name);
    if (value == NULL) {
        return 0;
    }
    /* buf = buf[0, start) + value + buf[end, len) */
    char *rest = strdup(lx->buf + t->end);
    if (rest == NULL) {
        return 0;
    }
    lx->len = t->start;
    lx->buf[lx->len] = '\0';
    append(lx, value);
    lx->alias_end = lx->len; /* no alias expansion inside the alias's own text */
    append(lx, rest);
    free(rest);
    lx->pos = t->start;
    return 1;
}
