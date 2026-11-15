#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "acsh.h"
#include "lex.h"

/* Recursive-descent parser for the POSIX shell grammar:
 *
 *   list      : and_or ((';' | '&' | newline) and_or)*
 *   and_or    : pipeline (('&&' | '||') linebreak pipeline)*
 *   pipeline  : ['!'] command ('|' linebreak command)*
 *   command   : simple_command
 *             | compound_command redirect*
 *             | NAME '(' ')' linebreak compound_command redirect*
 *   compound  : { list } | ( list ) | if | while | until | for | case
 *
 * At the top level a newline ends the command; inside a compound
 * command newlines separate commands like ';'. */

static const char *IF_USAGE    = "if CONDITION; then COMMANDS; fi";
static const char *WHILE_USAGE = "while CONDITION; do COMMANDS; done";
static const char *UNTIL_USAGE = "until CONDITION; do COMMANDS; done";
static const char *FOR_USAGE   = "for NAME in WORDS; do COMMANDS; done";
static const char *CASE_USAGE  = "case WORD in PATTERN) COMMANDS ;; esac";

typedef struct {
    Lexer *lx;
    Token  tok;
    int    have;      /* tok holds a peeked token */
    int    failed;    /* an error was reported */
    size_t last_end;  /* end of the last token taken */
} Parser;

/* ---- tokens ---- */

static Token *peek(Parser *p) {
    if (!p->have) {
        if (lex_next(p->lx, &p->tok) != 0) {
            p->failed = 1;
            p->tok.type = T_EOF;
            p->tok.text = NULL;
        }
        p->have = 1;
    }
    return &p->tok;
}

/* Takes the peeked token; the caller owns its text. */
static Token take(Parser *p) {
    peek(p);
    p->have = 0;
    p->last_end = p->tok.end;
    return p->tok;
}

static void drop(Parser *p) {
    Token t = take(p);
    free(t.text);
}

static int is_kw(Token *t, const char *word) {
    return t->type == T_WORD && !t->quoted && strcmp(t->text, word) == 0;
}

static int is_reserved(const char *w) {
    static const char *words[] = { "if", "then", "else", "elif", "fi", "do", "done",
                                   "case", "esac", "while", "until", "for", "in",
                                   "{", "}", "!", NULL };
    for (int i = 0; words[i] != NULL; i++) {
        if (strcmp(w, words[i]) == 0) return 1;
    }
    return 0;
}

/* Tokens that end a list inside a compound command. */
static int at_list_end(Parser *p) {
    Token *t = peek(p);
    if (p->failed) return 1;
    switch (t->type) {
    case T_RPAREN: case T_DSEMI: case T_SEMIAND: case T_EOF:
        return 1;
    case T_WORD:
        return is_kw(t, "then") || is_kw(t, "else") || is_kw(t, "elif") || is_kw(t, "fi") ||
               is_kw(t, "do") || is_kw(t, "done") || is_kw(t, "esac") || is_kw(t, "}");
    default:
        return 0;
    }
}

static void skip_newlines(Parser *p) {
    while (!p->failed && peek(p)->type == T_NEWLINE) {
        drop(p);
    }
}

static const char *describe(Token *t) {
    switch (t->type) {
    case T_WORD:      return t->text;
    case T_IO_NUMBER: return t->text;
    case T_NEWLINE:   return "newline";
    case T_EOF:       return "end of input";
    case T_SEMI:      return ";";
    case T_AMP:       return "&";
    case T_PIPE:      return "|";
    case T_AND_IF:    return "&&";
    case T_OR_IF:     return "||";
    case T_DSEMI:     return ";;";
    case T_SEMIAND:   return ";&";
    case T_LPAREN:    return "(";
    case T_RPAREN:    return ")";
    case T_LESS:      return "<";
    case T_GREAT:     return ">";
    case T_DGREAT:    return ">>";
    case T_LESSAND:   return "<&";
    case T_GREATAND:  return ">&";
    case T_LESSGREAT: return "<>";
    case T_CLOBBER:   return ">|";
    case T_DLESS:     return "<<";
    case T_DLESSDASH: return "<<-";
    case T_ANDGREAT:  return "&>";
    case T_ANDDGREAT: return "&>>";
    }
    return "?";
}

/* Reports an error at the current token, unless one was already reported. */
static void *fail(Parser *p, const char *usage, const char *msg) {
    if (!p->failed) {
        Token *t = peek(p);
        if (!p->failed) {
            lex_syntax_error(p->lx, t->start, usage, "%s", msg);
        }
        p->failed = 1;
    }
    return NULL;
}

static void *fail_unexpected(Parser *p) {
    if (!p->failed) {
        Token *t = peek(p);
        if (!p->failed) {
            lex_syntax_error(p->lx, t->start, NULL, "unexpected '%s'", describe(t));
        }
        p->failed = 1;
    }
    return NULL;
}

/* Consumes reserved word `kw`, or reports `msg`. */
static int expect_kw(Parser *p, const char *kw, const char *msg, const char *usage) {
    if (p->failed) return 0;
    if (!is_kw(peek(p), kw)) {
        fail(p, usage, msg);
        return 0;
    }
    drop(p);
    return 1;
}

/* ---- tree ---- */

static Node *node_new(NodeKind kind) {
    Node *n = calloc(1, sizeof(Node));
    if (n == NULL) {
        shell_error("out of memory\n");
        exit(2);
    }
    n->kind = kind;
    n->refs = 1;
    return n;
}

static Node *node_bin(NodeKind kind, Node *left, Node *right) {
    Node *n = node_new(kind);
    n->u.bin.left = left;
    n->u.bin.right = right;
    return n;
}

static void push_str(char ***arr, int *n, char *s) {
    char **bigger = realloc(*arr, (size_t)(*n + 1) * sizeof(char *));
    if (bigger == NULL) {
        shell_error("out of memory\n");
        exit(2);
    }
    *arr = bigger;
    (*arr)[(*n)++] = s;
}

static void free_strs(char **arr, int n) {
    for (int i = 0; i < n; i++) free(arr[i]);
    free(arr);
}

static void free_redirs(Redir *r) {
    while (r != NULL) {
        Redir *next = r->next;
        free(r->target);
        free(r);
        r = next;
    }
}

Node *node_ref(Node *n) {
    if (n != NULL) n->refs++;
    return n;
}

void node_unref(Node *n) {
    if (n == NULL || --n->refs > 0) {
        return;
    }
    free_redirs(n->redirs);
    free(n->text);
    switch (n->kind) {
    case N_SIMPLE:
        free_strs(n->u.simple.words, n->u.simple.nwords);
        free_strs(n->u.simple.assigns, n->u.simple.nassigns);
        break;
    case N_PIPELINE:
        for (int i = 0; i < n->u.pipe.ncmds; i++) node_unref(n->u.pipe.cmds[i]);
        free(n->u.pipe.cmds);
        break;
    case N_AND: case N_OR: case N_SEQ:
        node_unref(n->u.bin.left);
        node_unref(n->u.bin.right);
        break;
    case N_BACKGROUND: case N_SUBSHELL: case N_GROUP:
        node_unref(n->u.sub.body);
        break;
    case N_IF:
        node_unref(n->u.ifs.cond);
        node_unref(n->u.ifs.then_part);
        node_unref(n->u.ifs.else_part);
        break;
    case N_WHILE: case N_UNTIL:
        node_unref(n->u.loop.cond);
        node_unref(n->u.loop.body);
        break;
    case N_FOR:
        free(n->u.forl.var);
        free_strs(n->u.forl.words, n->u.forl.nwords);
        node_unref(n->u.forl.body);
        break;
    case N_CASE: {
        free(n->u.cases.subject);
        CaseItem *it = n->u.cases.items;
        while (it != NULL) {
            CaseItem *next = it->next;
            free_strs(it->patterns, it->npatterns);
            node_unref(it->body);
            free(it);
            it = next;
        }
        break;
    }
    case N_FUNCDEF:
        free(n->u.func.name);
        node_unref(n->u.func.body);
        break;
    }
    free(n);
}

/* ---- grammar ---- */

static Node *parse_list(Parser *p, int top);
static Node *parse_command(Parser *p);

static int is_name(const char *s) {
    if (!(isalpha((unsigned char)*s) || *s == '_')) return 0;
    for (s++; *s; s++) {
        if (!(isalnum((unsigned char)*s) || *s == '_')) return 0;
    }
    return 1;
}

/* NAME=value with an unquoted name: a variable assignment. */
static int is_assignment(const char *w) {
    const char *eq = strchr(w, '=');
    if (eq == NULL || eq == w) return 0;
    for (const char *s = w; s < eq; s++) {
        if (!(isalnum((unsigned char)*s) || *s == '_')) return 0;
    }
    return !isdigit((unsigned char)*w);
}

static int is_redirect_start(Token *t) {
    switch (t->type) {
    case T_IO_NUMBER: case T_LESS: case T_GREAT: case T_DGREAT: case T_LESSAND:
    case T_GREATAND: case T_LESSGREAT: case T_CLOBBER: case T_DLESS: case T_DLESSDASH:
    case T_ANDGREAT: case T_ANDDGREAT:
        return 1;
    default:
        return 0;
    }
}

static int parse_redirect(Parser *p, Redir **list) {
    int fd = -1;
    if (peek(p)->type == T_IO_NUMBER) {
        Token n = take(p);
        long v = strtol(n.text, NULL, 10);
        free(n.text);
        if (v > 9) {
            fail(p, NULL, "file descriptor numbers above 9 are not supported");
            return -1;
        }
        fd = (int)v;
    }
    Token op = take(p);
    RedirKind kind;
    int default_fd;
    switch (op.type) {
    case T_LESS:      kind = R_IN;     default_fd = 0; break;
    case T_GREAT:     kind = R_OUT;    default_fd = 1; break;
    case T_CLOBBER:   kind = R_CLOBBER; default_fd = 1; break;
    case T_DGREAT:    kind = R_APPEND; default_fd = 1; break;
    case T_LESSGREAT: kind = R_RDWR;   default_fd = 0; break;
    case T_LESSAND:   kind = R_DUP;    default_fd = 0; break;
    case T_GREATAND:  kind = R_DUP;    default_fd = 1; break;
    case T_DLESS:     kind = R_HEREDOC; default_fd = 0; break;
    case T_DLESSDASH: kind = R_HEREDOC; default_fd = 0; break;
    case T_ANDGREAT:  kind = R_OUT;    default_fd = 1; break;
    case T_ANDDGREAT: kind = R_APPEND; default_fd = 1; break;
    default:
        fail_unexpected(p);
        return -1;
    }
    if (peek(p)->type != T_WORD) {
        fail(p, NULL, "expected a file name after the redirection");
        return -1;
    }
    Token target = take(p);
    Redir *r = calloc(1, sizeof(Redir));
    if (r == NULL) {
        shell_error("out of memory\n");
        exit(2);
    }
    r->fd = (fd >= 0) ? fd : default_fd;
    r->kind = kind;
    if (kind == R_HEREDOC) {
        /* the delimiter is the word with its quotes removed; any quoting
         * makes the body literal. The body is read at the end of the line. */
        char *delim = target.text;
        char *o = delim;
        for (char *c = delim; *c; c++) {
            if (*c != SQ_START && *c != SQ_END && *c != DQ_START && *c != DQ_END) *o++ = *c;
        }
        *o = '\0';
        r->target = strdup("");
        lex_add_heredoc(p->lx, r, delim, target.quoted, op.type == T_DLESSDASH);
        free(delim);
    } else {
        r->target = target.text;
    }
    if (op.type == T_ANDGREAT || op.type == T_ANDDGREAT) {
        /* &>word and &>>word: stdout to word, then 2>&1 */
        if (fd >= 0) {
            free(r->target);
            free(r);
            fail(p, NULL, "a file descriptor number cannot come before '&>'");
            return -1;
        }
        Redir *dup = calloc(1, sizeof(Redir));
        if (dup == NULL) {
            shell_error("out of memory\n");
            exit(2);
        }
        dup->fd = 2;
        dup->kind = R_DUP;
        dup->target = strdup("1");
        r->next = dup;
    }
    while (*list != NULL) list = &(*list)->next; /* keep source order */
    *list = r;
    return 0;
}

static void parse_redirects(Parser *p, Node *n) {
    while (!p->failed && is_redirect_start(peek(p))) {
        if (parse_redirect(p, &n->redirs) != 0) return;
    }
}

static void add_word(Node *n, char *w) {
    if (n->u.simple.nwords == 0 && is_assignment(w)) {
        push_str(&n->u.simple.assigns, &n->u.simple.nassigns, w);
    } else {
        push_str(&n->u.simple.words, &n->u.simple.nwords, w);
    }
}

/* [[ ... ]]: between the brackets, && || ( ) < > are words for the
 * test builtin, not shell operators. */
static Node *parse_dbracket(Parser *p, char *open) {
    Node *n = node_new(N_SIMPLE);
    add_word(n, open);
    for (;;) {
        Token *t = peek(p);
        if (p->failed) break;
        const char *op = NULL;
        switch (t->type) {
        case T_AND_IF: op = "&&"; break;
        case T_OR_IF:  op = "||"; break;
        case T_LPAREN: op = "(";  break;
        case T_RPAREN: op = ")";  break;
        case T_LESS:   op = "<";  break;
        case T_GREAT:  op = ">";  break;
        case T_WORD: {
            Token w = take(p);
            int end = !w.quoted && strcmp(w.text, "]]") == 0;
            add_word(n, w.text);
            if (end) {
                parse_redirects(p, n);
                if (p->failed) { node_unref(n); return NULL; }
                return n;
            }
            continue;
        }
        default:
            fail(p, "[[ EXPRESSION ]]", "expected ']]' to close '[['");
            node_unref(n);
            return NULL;
        }
        drop(p);
        add_word(n, strdup(op));
    }
    node_unref(n);
    return NULL;
}

static Node *parse_simple(Parser *p, char *first_word) {
    Node *n = node_new(N_SIMPLE);
    if (first_word != NULL) {
        add_word(n, first_word);
    }
    while (!p->failed) {
        Token *t = peek(p);
        if (p->lx->alias_blank && t->start >= p->lx->alias_end) {
            /* POSIX: after an alias whose value ends in a blank, the
             * next word is also checked for an alias */
            p->lx->alias_blank = 0;
            if (t->type == T_WORD && lex_expand_alias(p->lx, t)) {
                free(t->text);
                p->have = 0;
                continue;
            }
        }
        if (is_redirect_start(t)) {
            if (parse_redirect(p, &n->redirs) != 0) break;
        } else if (t->type == T_WORD) {
            Token w = take(p);
            add_word(n, w.text);
        } else {
            break;
        }
    }
    if (!p->failed && n->u.simple.nwords == 0 && n->u.simple.nassigns == 0 && n->redirs == NULL) {
        fail_unexpected(p);
    }
    if (p->failed) {
        node_unref(n);
        return NULL;
    }
    return n;
}

/* A list that must not be empty, e.g. the body after 'then'. */
static Node *parse_body(Parser *p, const char *msg, const char *usage) {
    Node *n = parse_list(p, 0);
    if (n == NULL && !p->failed) {
        fail(p, usage, msg);
    }
    return n;
}

/* After 'if' or 'elif'. */
static Node *parse_if_rest(Parser *p, const char *kw) {
    Node *n = node_new(N_IF);
    char msg[80];
    snprintf(msg, sizeof(msg), "expected a condition after '%s'", kw);
    n->u.ifs.cond = parse_body(p, msg, IF_USAGE);
    snprintf(msg, sizeof(msg), "expected 'then' after the '%s' condition", kw);
    if (expect_kw(p, "then", msg, IF_USAGE)) {
        n->u.ifs.then_part = parse_body(p, "expected a command after 'then'", IF_USAGE);
    }
    if (!p->failed && is_kw(peek(p), "elif")) {
        drop(p);
        n->u.ifs.else_part = parse_if_rest(p, "elif");
    } else if (!p->failed && is_kw(peek(p), "else")) {
        drop(p);
        n->u.ifs.else_part = parse_body(p, "expected a command after 'else'", IF_USAGE);
    }
    return n;
}

static Node *parse_if(Parser *p) {
    drop(p); /* if */
    Node *n = parse_if_rest(p, "if");
    expect_kw(p, "fi", "expected 'fi' to close the 'if'", IF_USAGE);
    return n;
}

static Node *parse_loop(Parser *p, NodeKind kind) {
    const char *kw = (kind == N_WHILE) ? "while" : "until";
    const char *usage = (kind == N_WHILE) ? WHILE_USAGE : UNTIL_USAGE;
    char msg[80];
    drop(p);
    Node *n = node_new(kind);
    snprintf(msg, sizeof(msg), "expected a condition after '%s'", kw);
    n->u.loop.cond = parse_body(p, msg, usage);
    snprintf(msg, sizeof(msg), "expected 'do' after the '%s' condition", kw);
    if (expect_kw(p, "do", msg, usage)) {
        n->u.loop.body = parse_body(p, "expected a command after 'do'", usage);
    }
    snprintf(msg, sizeof(msg), "expected 'done' to close the '%s' loop", kw);
    expect_kw(p, "done", msg, usage);
    return n;
}

static Node *parse_for(Parser *p) {
    drop(p); /* for */
    Node *n = node_new(N_FOR);
    Token *t = peek(p);
    if (p->failed || t->type != T_WORD || t->quoted || !is_name(t->text)) {
        fail(p, FOR_USAGE, "expected a variable name after 'for'");
        return n;
    }
    Token name = take(p);
    n->u.forl.var = name.text;
    skip_newlines(p);
    if (!p->failed && is_kw(peek(p), "in")) {
        drop(p);
        n->u.forl.has_in = 1;
        while (!p->failed && peek(p)->type == T_WORD) {
            Token w = take(p);
            push_str(&n->u.forl.words, &n->u.forl.nwords, w.text);
        }
        if (!p->failed) {
            TokType sep = peek(p)->type;
            if (sep != T_SEMI && sep != T_NEWLINE) {
                fail(p, FOR_USAGE, "expected ';' or a newline after the 'for' word list");
                return n;
            }
            drop(p);
        }
    } else if (!p->failed && peek(p)->type == T_SEMI) {
        drop(p);
    }
    skip_newlines(p);
    if (expect_kw(p, "do", "expected 'do' after the 'for' word list", FOR_USAGE)) {
        n->u.forl.body = parse_body(p, "expected a command after 'do'", FOR_USAGE);
    }
    expect_kw(p, "done", "expected 'done' to close the 'for' loop", FOR_USAGE);
    return n;
}

static Node *parse_case(Parser *p) {
    drop(p); /* case */
    Node *n = node_new(N_CASE);
    if (p->failed || peek(p)->type != T_WORD) {
        fail(p, CASE_USAGE, "expected a word after 'case'");
        return n;
    }
    Token subject = take(p);
    n->u.cases.subject = subject.text;
    skip_newlines(p);
    if (!expect_kw(p, "in", "expected 'in' after the 'case' word", CASE_USAGE)) {
        return n;
    }
    skip_newlines(p);
    CaseItem **tail = &n->u.cases.items;
    while (!p->failed) {
        if (is_kw(peek(p), "esac")) {
            drop(p);
            return n;
        }
        if (peek(p)->type == T_EOF) {
            fail(p, CASE_USAGE, "expected 'esac' to close the 'case'");
            break;
        }
        CaseItem *item = calloc(1, sizeof(CaseItem));
        if (item == NULL) {
            shell_error("out of memory\n");
            exit(2);
        }
        *tail = item;
        tail = &item->next;
        if (peek(p)->type == T_LPAREN) {
            drop(p);
        }
        for (;;) {
            if (peek(p)->type != T_WORD) {
                fail(p, CASE_USAGE, "expected a pattern in the 'case'");
                return n;
            }
            Token pat = take(p);
            push_str(&item->patterns, &item->npatterns, pat.text);
            if (peek(p)->type != T_PIPE) break;
            drop(p);
        }
        if (peek(p)->type != T_RPAREN) {
            fail(p, CASE_USAGE, "expected ')' after the case pattern");
            return n;
        }
        drop(p);
        item->body = parse_list(p, 0);
        if (p->failed) break;
        if (peek(p)->type == T_DSEMI || peek(p)->type == T_SEMIAND) {
            item->fallthrough = (peek(p)->type == T_SEMIAND); /* POSIX.1-2024 */
            drop(p);
            skip_newlines(p);
        } else if (!is_kw(peek(p), "esac")) {
            fail(p, CASE_USAGE, "expected ';;', ';&' or 'esac' after the case clause");
        }
    }
    return n;
}

static Node *parse_group(Parser *p) {
    drop(p); /* { */
    Node *n = node_new(N_GROUP);
    n->u.sub.body = parse_body(p, "expected a command after '{'", NULL);
    expect_kw(p, "}", "expected '}' to close the '{' group", NULL);
    return n;
}

static Node *parse_subshell(Parser *p) {
    drop(p); /* ( */
    Node *n = node_new(N_SUBSHELL);
    n->u.sub.body = parse_body(p, "expected a command after '('", NULL);
    if (!p->failed) {
        if (peek(p)->type != T_RPAREN) {
            fail(p, NULL, "expected ')' to close the '(' subshell");
        } else {
            drop(p);
        }
    }
    return n;
}

/* The compound command starting at the current token, or NULL if the
 * token does not start one. */
static Node *parse_compound(Parser *p) {
    Token *t = peek(p);
    Node *n = NULL;
    if (t->type == T_LPAREN)     n = parse_subshell(p);
    else if (is_kw(t, "{"))      n = parse_group(p);
    else if (is_kw(t, "if"))     n = parse_if(p);
    else if (is_kw(t, "while"))  n = parse_loop(p, N_WHILE);
    else if (is_kw(t, "until"))  n = parse_loop(p, N_UNTIL);
    else if (is_kw(t, "for"))    n = parse_for(p);
    else if (is_kw(t, "case"))   n = parse_case(p);
    else return NULL;
    parse_redirects(p, n);
    return n;
}

/* NAME() BODY, or with `keyword` (after `function`): NAME [()] BODY. */
static Node *parse_funcdef(Parser *p, char *name, int keyword) {
    if (!keyword || peek(p)->type == T_LPAREN) {
        drop(p); /* ( */
        if (peek(p)->type != T_RPAREN) {
            free(name);
            return fail(p, NULL, "expected ')' after '(' in the function definition");
        }
        drop(p);
    }
    skip_newlines(p);
    Node *body = p->failed ? NULL : parse_compound(p);
    if (body == NULL) {
        char msg[160];
        snprintf(msg, sizeof(msg), "expected '{' to start the body of function '%s'", name);
        free(name);
        return fail(p, "NAME() { COMMANDS; }", msg);
    }
    Node *n = node_new(N_FUNCDEF);
    n->u.func.name = name;
    n->u.func.body = body;
    return n;
}

static Node *parse_command_at(Parser *p);

static Node *parse_command(Parser *p) {
    size_t start = peek(p)->start;
    Node *n = parse_command_at(p);
    if (n != NULL) {
        n->lineno = lex_line_at(p->lx, start);
    }
    return n;
}

static Node *parse_command_at(Parser *p) {
    for (;;) {
        Token *t = peek(p);
        if (p->failed) return NULL;
        /* an alias is expanded where a command name is expected */
        if (t->type == T_WORD && !t->quoted && !is_reserved(t->text) &&
            lex_expand_alias(p->lx, t)) {
            free(t->text);
            p->have = 0;
            continue;
        }
        break;
    }
    Node *n = parse_compound(p);
    if (n != NULL || p->failed) {
        return n;
    }
    Token *t = peek(p);
    if (t->type == T_WORD && !t->quoted && is_reserved(t->text) && !is_kw(t, "!")) {
        return fail_unexpected(p);
    }
    if (t->type == T_WORD) {
        Token w = take(p);
        if (!w.quoted && strcmp(w.text, "[[") == 0) {
            return parse_dbracket(p, w.text);
        }
        if (!w.quoted && is_name(w.text) && peek(p)->type == T_LPAREN) {
            return parse_funcdef(p, w.text, 0);
        }
        if (!w.quoted && strcmp(w.text, "function") == 0 && peek(p)->type == T_WORD) {
            /* function NAME [()] BODY, as in ash, bash and ksh */
            Token name = take(p);
            free(w.text);
            if (name.quoted || !is_name(name.text)) {
                free(name.text);
                return fail(p, "function NAME { COMMANDS; }", "expected a function name after 'function'");
            }
            return parse_funcdef(p, name.text, 1);
        }
        return parse_simple(p, w.text);
    }
    return parse_simple(p, NULL);
}

static Node *parse_pipeline(Parser *p) {
    int bang = 0;
    if (is_kw(peek(p), "!")) {
        drop(p);
        bang = 1;
    }
    size_t start = peek(p)->start;
    Node *n = node_new(N_PIPELINE);
    n->u.pipe.bang = bang;
    for (;;) {
        Node *cmd = parse_command(p);
        if (cmd == NULL) {
            node_unref(n);
            return NULL;
        }
        Node **bigger = realloc(n->u.pipe.cmds, (size_t)(n->u.pipe.ncmds + 1) * sizeof(Node *));
        if (bigger == NULL) {
            shell_error("out of memory\n");
            exit(2);
        }
        n->u.pipe.cmds = bigger;
        n->u.pipe.cmds[n->u.pipe.ncmds++] = cmd;
        if (p->failed || peek(p)->type != T_PIPE) break;
        drop(p);
        skip_newlines(p);
    }
    n->text = lex_source(p->lx, start, p->last_end);
    return n;
}

static Node *parse_and_or(Parser *p) {
    Node *n = parse_pipeline(p);
    while (n != NULL && !p->failed) {
        TokType t = peek(p)->type;
        if (t != T_AND_IF && t != T_OR_IF) break;
        drop(p);
        skip_newlines(p);
        Node *right = parse_pipeline(p);
        n = node_bin(t == T_AND_IF ? N_AND : N_OR, n, right);
        if (right == NULL) break;
    }
    return n;
}

/* top: a newline ends the list (a complete command). Otherwise this is
 * the list inside a compound command, where newlines separate commands
 * and a closing keyword or token ends it; it may be empty (NULL). */
static Node *parse_list(Parser *p, int top) {
    if (!top) {
        skip_newlines(p);
        if (at_list_end(p)) return NULL;
    }
    Node *list = NULL;
    for (;;) {
        size_t start = peek(p)->start;
        Node *item = parse_and_or(p);
        if (item == NULL || p->failed) {
            node_unref(item);
            node_unref(list);
            return NULL;
        }
        TokType t = peek(p)->type;
        int separated = 1;
        if (t == T_AMP) {
            size_t end = p->last_end;  /* the job's text, without the & (as ash shows it) */
            drop(p);
            Node *bg = node_new(N_BACKGROUND);
            bg->u.sub.body = item;
            bg->text = lex_source(p->lx, start, end);
            item = bg;
        } else if (t == T_SEMI) {
            drop(p);
        } else {
            separated = 0;
        }
        list = (list == NULL) ? item : node_bin(N_SEQ, list, item);
        if (p->failed) break;

        t = peek(p)->type;
        if (top) {
            /* a newline ends a complete command; after ';' or '&'
             * another command may follow on the same line */
            if (!separated || t == T_NEWLINE || t == T_EOF) break;
            continue;
        }
        if (!separated && t != T_NEWLINE) break; /* the caller expects a closing keyword */
        skip_newlines(p);
        if (at_list_end(p)) break;
    }
    if (p->failed) {
        node_unref(list);
        return NULL;
    }
    return list;
}

ParseResult parse_next(Lexer *lx, Node **out) {
    Parser p = { lx, { T_EOF, NULL, 0, 0, 0 }, 0, 0, 0 };
    *out = NULL;
    lex_begin_command(lx);

    Token *t = peek(&p);
    if (!p.failed && t->type == T_EOF) {
        return PARSE_EOF;
    }
    if (!p.failed && t->type == T_NEWLINE) {
        drop(&p);
        return PARSE_OK; /* empty line */
    }

    Node *n = p.failed ? NULL : parse_list(&p, 1);
    if (!p.failed) {
        t = peek(&p);
        if (t->type == T_NEWLINE) {
            drop(&p);
        } else if (t->type != T_EOF) {
            fail_unexpected(&p);
        }
    }
    if (p.have) {
        free(p.tok.text);
    }
    if (p.failed) {
        node_unref(n);
        lex_discard(lx);
        return PARSE_ERROR;
    }
    *out = n;
    return PARSE_OK;
}
