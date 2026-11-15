#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "acsh.h"
#include "lex.h"

/* The lexer turns input text into tokens. Words are decoded into the
 * marked form expand.c expects:
 *
 *   'text', \c, and \$ \" \\ \` inside "..."  ->  SQ_START text SQ_END
 *   "text"                                   ->  DQ_START text DQ_END
 *   $(...), $((...)), `...`, ${...}          ->  copied verbatim (raw)
 *
 * When a quote, substitution or construct is still open at the end of
 * the buffer, the lexer asks its LineSource for another line; that is
 * how one command can span several lines. */

#define END (-1)

typedef struct PendingDoc {
    Redir             *redir;
    char              *delim;
    int                quoted;
    int                strip_tabs;
    struct PendingDoc *next;
} PendingDoc;

static void drop_heredocs(Lexer *lx);

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
        if (!shell_interrupted()) lx->at_eof = 1;  /* else Ctrl-C cancelled a line: not the end */
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
    if (name != NULL && strcmp(name, "-c") == 0) {
        lx->base_line = -1; /* ash numbers -c lines from 0 */
    }
    return lx;
}

static void drop_active_aliases(Lexer *lx, size_t before) {
    while (lx->nactive > 0 && lx->active[lx->nactive - 1].end <= before) {
        free(lx->active[--lx->nactive].name);
    }
}

void lexer_free(Lexer *lx) {
    if (lx != NULL) {
        drop_active_aliases(lx, (size_t)-1);
        drop_heredocs(lx);
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
    lx->line_pos = 0;
    lx->line_count = 0;
    lx->alias_blank = 0;
    drop_active_aliases(lx, (size_t)-1);
    lx->new_command = 1;
}

static void drop_heredocs(Lexer *lx) {
    while (lx->docs != NULL) {
        PendingDoc *d = lx->docs;
        lx->docs = d->next;
        free(d->delim);
        free(d);
    }
}

void lex_discard(Lexer *lx) {
    lx->pos = lx->len;
    lx->new_command = 1;  /* what comes next starts a new command (PS1, not PS2) */
    drop_heredocs(lx); /* their redirections were freed with the failed command */
}

void lex_discard_input(Lexer *lx) {
    lex_discard(lx);
}

int lex_line_at(Lexer *lx, size_t pos) {
    if (pos > lx->len) pos = lx->len;
    if (pos < lx->line_pos) {         /* behind the cache (text was replaced): recount */
        lx->line_pos = 0;
        lx->line_count = 0;
    }
    /* count only the new part: a long script stays linear */
    for (size_t i = lx->line_pos; i < pos; i++) {
        if (lx->buf[i] == '\n') lx->line_count++;
    }
    lx->line_pos = pos;
    return lx->base_line + 1 + lx->line_count;
}

char *lex_source(Lexer *lx, size_t start, size_t end) {
    return strndup(lx->buf + start, end - start);
}

void lex_syntax_error(Lexer *lx, size_t pos, const char *usage, const char *fmt, ...) {
    va_list ap;
    if (shell_interrupted()) {
        return;   /* Ctrl-C cancelled the command being typed: no error to report */
    }
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

void paren_scan_init(ParenScan *ps) {
    memset(ps, 0, sizeof(*ps));
    ps->depth = 1;
    ps->cmd_start = 1;
}

/* A finished word: tracks `case WORD in` and `esac`, and whether the
 * next word starts a command. */
static void paren_word(ParenScan *ps) {
    if (ps->wlen == 0) return;
    const char *w = ps->wlen < (int)sizeof(ps->word) ? ps->word : "";
    ps->word[ps->wlen < (int)sizeof(ps->word) ? ps->wlen : 0] = '\0';
    ps->wlen = 0;
    int top = ps->ncase - 1;
    if (top >= 0 && ps->case_at[top] == ps->depth && ps->case_state[top] < 2) {
        if (ps->case_state[top] == 0) ps->case_state[top] = 1;     /* the subject */
        else if (strcmp(w, "in") == 0) ps->case_state[top] = 2;
        ps->cmd_start = 0;
        return;
    }
    if (ps->cmd_start && strcmp(w, "case") == 0 && ps->ncase < 16) {
        ps->case_at[ps->ncase] = ps->depth;
        ps->case_state[ps->ncase++] = 0;
        ps->cmd_start = 0;
        return;
    }
    if (ps->cmd_start && strcmp(w, "esac") == 0 && top >= 0 && ps->case_at[top] == ps->depth) {
        ps->ncase--;
        ps->cmd_start = 0;
        return;
    }
    static const char *reserved[] = { "then", "do", "else", "elif", "if", "while", "until", "{", "}", "!", "time", NULL };
    int is_reserved = 0;
    for (int i = 0; reserved[i]; i++) if (strcmp(w, reserved[i]) == 0) is_reserved = 1;
    ps->cmd_start = ps->cmd_start && is_reserved;
}

int paren_scan(ParenScan *ps, int c) {
    if (isalnum(c) || c == '_' || c == '{' || c == '}' || c == '!' || c == '$' || c == '-' ||
        c == '.' || c == '/' || c == '=' || c == '"' || c == '\'' || c == '\\' || c == '*' ||
        c == '?' || c == '[' || c == ']' || c == '~' || c == ':' || c == '@' || c == '%' ||
        c == '+' || c == ',' || c == '^' || (unsigned char)c >= 0x80) {
        if (ps->wlen < (int)sizeof(ps->word) - 1) ps->word[ps->wlen] = (char)c;
        ps->wlen++;
        return 0;
    }
    paren_word(ps);
    int top = ps->ncase - 1;
    if (c == '(') {
        ps->depth++;
        ps->cmd_start = 1;
    } else if (c == ')') {
        if (top >= 0 && ps->case_at[top] == ps->depth && ps->case_state[top] == 2) {
            ps->cmd_start = 1; /* the end of a case pattern: its body follows */
            return 0;
        }
        if (--ps->depth == 0) return 1;
        ps->cmd_start = 0;
    } else if (c == ';' || c == '&' || c == '|' || c == '\n') {
        ps->cmd_start = 1;
    }
    return 0;
}

/* Copies a $( ... ) region verbatim; nested parentheses and quotes are
 * tracked so the right ')' ends it. */
static int copy_paren_subst(Lexer *lx, Str *out, size_t start) {
    put(out, (char)ch(lx, 0)); /* $ */
    put(out, (char)ch(lx, 1)); /* ( */
    lx->pos += 2;
    ParenScan ps;
    paren_scan_init(&ps);
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
            if (c == '\\' && (quote == '"' || quote == 'D') && ch(lx, 0) != END) {
                put(out, (char)ch(lx, 0));
                lx->pos++;
            } else if (c == (quote == 'D' ? '\'' : quote)) {
                quote = 0;
            }
        } else if (c == '\\' && ch(lx, 0) != END) {
            put(out, (char)ch(lx, 0));
            lx->pos++;
            paren_scan(&ps, 'x'); /* an escaped character is part of a word */
        } else if (c == '\'' && out->len >= 2 && out->s[out->len - 2] == '$') {
            quote = 'D'; /* $'...': backslash escapes the quote */
            paren_scan(&ps, c);
        } else if (c == '\'' || c == '"') {
            quote = c;
            paren_scan(&ps, c);
        } else if (paren_scan(&ps, c)) {
            return 0;
        }
        if (ps.depth > ACSH_MAX_NESTING) {
            lex_syntax_error(lx, start, NULL, "parentheses nested more than %d deep", ACSH_MAX_NESTING);
            return -1;
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
        } else if (c == '{' && ++depth > ACSH_MAX_NESTING) {
            lex_syntax_error(lx, start, NULL, "${...} nested more than %d deep", ACSH_MAX_NESTING);
            return -1;
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

/* Appends code point cp as UTF-8. */
static void put_utf8(Str *out, unsigned long cp) {
    if (cp < 0x80) {
        put(out, (char)cp);
    } else if (cp < 0x800) {
        put(out, (char)(0xc0 | (cp >> 6)));
        put(out, (char)(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        put(out, (char)(0xe0 | (cp >> 12)));
        put(out, (char)(0x80 | ((cp >> 6) & 0x3f)));
        put(out, (char)(0x80 | (cp & 0x3f)));
    } else {
        put(out, (char)(0xf0 | ((cp >> 18) & 0x07)));
        put(out, (char)(0x80 | ((cp >> 12) & 0x3f)));
        put(out, (char)(0x80 | ((cp >> 6) & 0x3f)));
        put(out, (char)(0x80 | (cp & 0x3f)));
    }
}

static int hex_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* $'...' (ANSI-C quoting), with lx->pos at the '$': the escapes of
 * bash -- \a \b \e \E \f \n \r \t \v \\ \' \" \? \NNN \xHH \uHHHH
 * \UHHHHHHHH \cX -- become the characters they name, and the result is
 * taken literally. As in bash, a \0 ends the string. */
static int scan_dollar_single(Lexer *lx, Str *out) {
    size_t here = lx->pos;
    lx->pos += 2;
    put(out, SQ_START);
    int ended = 0; /* after \0: the rest is dropped */
    for (;;) {
        int c = ch(lx, 0);
        if (c == END) {
            return unterminated(lx, here, "no closing ' found for $'",
                                "every $' must be matched by a ' later on");
        }
        lx->pos++;
        if (c == '\'') break;
        Str tmp = { NULL, 0, 0 };
        Str *o = ended ? &tmp : out;
        if (c != '\\' || ch(lx, 0) == END) {
            put(o, (char)c);
            free(tmp.s);
            continue;
        }
        int e = ch(lx, 0);
        lx->pos++;
        unsigned long v = 0;
        int n = 0;
        switch (e) {
        case 'a': put(o, '\a'); break;
        case 'b': put(o, '\b'); break;
        case 'e': case 'E': put(o, '\033'); break;
        case 'f': put(o, '\f'); break;
        case 'n': put(o, '\n'); break;
        case 'r': put(o, '\r'); break;
        case 't': put(o, '\t'); break;
        case 'v': put(o, '\v'); break;
        case '\\': case '\'': case '"': case '?': put(o, (char)e); break;
        case 'c':
            if (ch(lx, 0) != END) {
                put(o, (char)(ch(lx, 0) & 0x1f));
                lx->pos++;
            }
            break;
        case 'x': case 'u': case 'U': {
            int max = (e == 'x') ? 2 : (e == 'u') ? 4 : 8;
            for (; n < max && hex_value(ch(lx, 0)) >= 0; n++, lx->pos++) v = v * 16 + (unsigned long)hex_value(ch(lx, 0));
            if (n == 0) { put(o, '\\'); put(o, (char)e); break; }
            if (e == 'x') {
                if (v == 0) ended = 1;
                else put(o, (char)v);
            } else if (v == 0) {
                ended = 1;
            } else {
                put_utf8(o, v);
            }
            break;
        }
        default:
            if (e >= '0' && e <= '7') {
                v = (unsigned long)(e - '0');
                for (n = 1; n < 3 && ch(lx, 0) >= '0' && ch(lx, 0) <= '7'; n++, lx->pos++) v = v * 8 + (unsigned long)(ch(lx, 0) - '0');
                if ((v & 0xff) == 0) ended = 1;
                else put(o, (char)(v & 0xff));
            } else {
                put(o, '\\'); /* an unknown escape stays as typed */
                put(o, (char)e);
            }
        }
        free(tmp.s);
    }
    put(out, SQ_END);
    return 0;
}

/* Copies a <( ... ) or >( ... ) process substitution verbatim; the
 * expander runs it. */
static int copy_proc_subst(Lexer *lx, Str *out, size_t start) {
    return copy_paren_subst(lx, out, start); /* copies the '<' or '>' as it would '$' */
}

/* Decodes one word (or, with `whole`, all remaining text) into marked
 * form. Sets *quoted if any quoting or escaping was seen. */
static int scan_text(Lexer *lx, Str *out, int whole, int *quoted) {
    for (;;) {
        int c = ch(lx, 0);
        if ((c == '<' || c == '>') && ch(lx, 1) == '(') {
            *quoted = 1; /* never an alias or a reserved word */
            if (copy_proc_subst(lx, out, lx->pos) != 0) return -1;
            continue;
        }
        if (c == END || (!whole && is_word_end(c))) {
            return 0;
        }
        size_t here = lx->pos;
        if (c == '$' && ch(lx, 1) == '\'') {
            *quoted = 1;
            if (scan_dollar_single(lx, out) != 0) return -1;
            continue;
        }
        if (c == '\\') {
            int d = ch(lx, 1);
            if (d == '\n') {           /* line continuation */
                lx->pos += 2;
                continue;
            }
            if (d == END) {            /* lone backslash at the very end */
                put(out, '\\');
                lx->pos++;
                continue;
            }
            *quoted = 1;
            put_literal(out, (char)d);
            lx->pos += 2;
        } else if (c == '\'') {
            *quoted = 1;
            lx->pos++;
            put(out, SQ_START);
            for (;;) {
                int d = ch(lx, 0);
                if (d == END) {
                    return unterminated(lx, here, "no closing ' found",
                                        "every ' must be matched by another ' later on");
                }
                lx->pos++;
                if (d == '\'') break;
                put(out, (char)d);
            }
            put(out, SQ_END);
        } else if (c == '"') {
            *quoted = 1;
            lx->pos++;
            put(out, DQ_START);
            for (;;) {
                int d = ch(lx, 0);
                if (d == END) {
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
                    } else if (e == '$' || e == '"' || e == '\\' || e == '`') {
                        put_literal(out, (char)e);
                        lx->pos += 2;
                    } else {
                        put(out, '\\');        /* other backslashes stay as they are */
                        lx->pos++;
                    }
                } else if (d == '$' && ch(lx, 1) == '(') {
                    if (copy_paren_subst(lx, out, lx->pos) != 0) return -1;
                } else if (d == '$' && ch(lx, 1) == '{') {
                    if (copy_brace_param(lx, out, lx->pos) != 0) return -1;
                } else if (d == '`') {
                    if (copy_backticks(lx, out, lx->pos) != 0) return -1;
                } else {
                    put(out, (char)d);
                    lx->pos++;
                }
            }
            put(out, DQ_END);
        } else if (c == '$' && ch(lx, 1) == '(') {
            if (copy_paren_subst(lx, out, here) != 0) return -1;
        } else if (c == '$' && ch(lx, 1) == '{') {
            if (copy_brace_param(lx, out, here) != 0) return -1;
        } else if (c == '`') {
            if (copy_backticks(lx, out, here) != 0) return -1;
        } else {
            put(out, (char)c);
            lx->pos++;
        }
    }
}

static int scan_word(Lexer *lx, Token *t) {
    Str out = { NULL, 0, 0 };
    put(&out, 'x');  /* make sure the string exists, even for '' */
    out.len = 0;
    out.s[0] = '\0';
    t->type = T_WORD;
    t->quoted = 0;
    if (scan_text(lx, &out, 0, &t->quoted) != 0) {
        free(out.s);
        return -1;
    }
    t->text = out.s;
    return 0;
}

char *lex_mark(const char *raw, int dquoted) {
    Lexer *lx = lexer_new(raw, NULL, NULL, NULL);
    Str out = { NULL, 0, 0 };
    put(&out, 'x');
    out.len = 0;
    out.s[0] = '\0';
    int quoted = 0;
    if (dquoted) put(&out, DQ_START);
    int r = scan_text(lx, &out, 1, &quoted);
    if (dquoted) put(&out, DQ_END);
    lexer_free(lx);
    if (r != 0) {
        free(out.s);
        return NULL;
    }
    return out.s;
}

/* ---- here-documents ---- */

void lex_add_heredoc(Lexer *lx, Redir *r, const char *delim, int quoted, int strip_tabs) {
    PendingDoc *d = calloc(1, sizeof(PendingDoc));
    if (d == NULL) {
        fprintf(stderr, "acsh: out of memory\n");
        exit(2);
    }
    d->redir = r;
    d->delim = strdup(delim);
    d->quoted = quoted;
    d->strip_tabs = strip_tabs;
    PendingDoc **tail = &lx->docs;
    while (*tail != NULL) tail = &(*tail)->next;
    *tail = d;
}

/* A body for an unquoted delimiter: expanded like the inside of double
 * quotes, except that " is an ordinary character. \$ \` \\ lose their
 * backslash, \newline disappears, other backslashes stay. */
static char *mark_heredoc(const char *body) {
    Lexer *lx = lexer_new(body, NULL, NULL, NULL);
    Str out = { NULL, 0, 0 };
    put(&out, DQ_START);
    for (;;) {
        int c = ch(lx, 0);
        if (c == END) break;
        if (c == '\\') {
            int d = ch(lx, 1);
            if (d == '\n') { lx->pos += 2; continue; }
            if (d == '$' || d == '`' || d == '\\') { put_literal(&out, (char)d); lx->pos += 2; continue; }
            put(&out, '\\');
            lx->pos++;
        } else if (c == '$' && ch(lx, 1) == '(') {
            if (copy_paren_subst(lx, &out, lx->pos) != 0) break;
        } else if (c == '$' && ch(lx, 1) == '{') {
            if (copy_brace_param(lx, &out, lx->pos) != 0) break;
        } else if (c == '`') {
            if (copy_backticks(lx, &out, lx->pos) != 0) break;
        } else {
            put(&out, (char)c);
            lx->pos++;
        }
    }
    put(&out, DQ_END);
    lexer_free(lx);
    return out.s;
}

/* A quoted delimiter's body: every character literal. */
static char *mark_literal(const char *body) {
    Str out = { NULL, 0, 0 };
    put(&out, SQ_START);
    for (const char *s = body; *s; s++) put(&out, *s);
    put(&out, SQ_END);
    return out.s;
}

/* Called right after a newline token: reads the bodies of the pending
 * here-documents, one line at a time, from the input that follows. */
static void read_heredocs(Lexer *lx) {
    while (lx->docs != NULL) {
        PendingDoc *d = lx->docs;
        lx->docs = d->next;
        Str body = { NULL, 0, 0 };
        put(&body, 'x');
        body.len = 0;
        body.s[0] = '\0';
        for (;;) {
            if (ch(lx, 0) == END) break;   /* end of input ends the body */
            size_t start = lx->pos;
            while (ch(lx, 0) != END && ch(lx, 0) != '\n') lx->pos++;
            size_t end = lx->pos;
            if (ch(lx, 0) == '\n') lx->pos++;
            const char *line = lx->buf + start;
            size_t len = end - start;
            if (d->strip_tabs) {
                while (len > 0 && *line == '\t') { line++; len--; }
            }
            if (len == strlen(d->delim) && strncmp(line, d->delim, len) == 0) break;
            for (size_t i = 0; i < len; i++) put(&body, line[i]);
            put(&body, '\n');
        }
        free(d->redir->target); /* the empty placeholder set by the parser */
        d->redir->target = d->quoted ? mark_literal(body.s) : mark_heredoc(body.s);
        free(body.s);
        free(d->delim);
        free(d);
    }
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
        if ((after == '<' || after == '>') && ch(lx, n + 1) != '(') { /* 4<(cmd) is a word */
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
    int e = ((c == '<' && d == '<') || (c == '&' && d == '>')) ? ch(lx, 2) : END;
    if ((c == '<' || c == '>') && d == '(') {
        if (scan_word(lx, t) != 0) return -1; /* <(cmd) >(cmd) */
        t->end = lx->pos;
        return 0;
    }
    int len = 1;
    switch (c) {
    case '\n':
        t->type = T_NEWLINE;
        lx->pos++;
        t->end = lx->pos;
        if (lx->docs != NULL) read_heredocs(lx);
        return 0;
    case ';':
        if (d == ';') { t->type = T_DSEMI;  len = 2; }
        else if (d == '&') { t->type = T_SEMIAND; len = 2; } /* ;& in case */
        else t->type = T_SEMI;
        break;
    case '&':
        if (d == '&') { t->type = T_AND_IF; len = 2; }
        else if (d == '>' && e == '>') { t->type = T_ANDDGREAT; len = 3; } /* &>> */
        else if (d == '>') { t->type = T_ANDGREAT; len = 2; }              /* &> */
        else t->type = T_AMP;
        break;
    case '|':  if (d == '|') { t->type = T_OR_IF;  len = 2; } else t->type = T_PIPE; break;
    case '(':  t->type = T_LPAREN; break;
    case ')':  t->type = T_RPAREN; break;
    case '<':
        if (d == '<' && e == '<') { t->type = T_TLESS; len = 3; }          /* <<< here-string */
        else if (d == '<' && e == '-') { t->type = T_DLESSDASH; len = 3; }
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
    if (t->type != T_WORD || t->quoted) {
        return 0;
    }
    char *name = lex_source(lx, t->start, t->end);
    const char *value = (name != NULL) ? alias_get(name) : NULL;
    /* an alias is not expanded again inside its own text (POSIX), so
     * alias ls='ls -F' ends; other aliases in that text still are */
    drop_active_aliases(lx, t->start + 1);
    int busy = (lx->nactive == (int)(sizeof(lx->active) / sizeof(lx->active[0])));
    for (int i = 0; value != NULL && i < lx->nactive; i++) {
        if (strcmp(lx->active[i].name, name) == 0) busy = 1;
    }
    if (value == NULL || busy) {
        free(name);
        return 0;
    }
    /* buf = buf[0, start) + value + buf[end, len) */
    char *rest = strdup(lx->buf + t->end);
    if (rest == NULL) {
        return 0;
    }
    lx->len = t->start;
    lx->buf[lx->len] = '\0';
    if (lx->line_pos > t->start) {    /* the text after start changes */
        lx->line_pos = 0;
        lx->line_count = 0;
    }
    append(lx, value);
    size_t vlen = strlen(value);
    size_t delta = vlen - (t->end - t->start); /* wraps when shrinking; adds back correctly */
    /* the word is replaced: text of enclosing aliases grows or shrinks */
    for (int i = 0; i < lx->nactive; i++) lx->active[i].end += delta;
    lx->active[lx->nactive].name = name;
    lx->active[lx->nactive++].end = t->start + vlen;
    if (vlen > 0 && (value[vlen - 1] == ' ' || value[vlen - 1] == '\t')) {
        lx->alias_blank = 1;               /* check the word after this text */
        lx->alias_end = t->start + vlen;
    } else if (lx->alias_blank && t->start < lx->alias_end) {
        lx->alias_end += delta;            /* inside text that ended in a blank: keep it */
    } else {
        lx->alias_blank = 0;
        lx->alias_end = t->start + vlen;
    }
    append(lx, rest);
    free(rest);
    lx->pos = t->start;
    return 1;
}
