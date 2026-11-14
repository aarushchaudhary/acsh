#ifndef ACSH_SYNTAX_H
#define ACSH_SYNTAX_H

/* acsh's reader, lexer, parser and syntax tree.
 *
 *   input lines -> lexer (tokens) -> parser (syntax tree) -> exec.c
 *
 * One complete command (everything up to an unquoted newline that ends
 * it) is parsed, run, and freed before the next is read, so an alias or
 * function defined on one line is available on the next, as in ash.
 * Words keep the marker bytes below so expansion (env.c, glob.c) knows
 * which characters were quoted. */

#include <stddef.h>

/* Marker bytes inside a word (see lexer.c). */
#define SQ_START       '\x01'  /* start of quoted text: no expansion, no globbing */
#define SQ_END         '\x02'  /* end of quoted text */
#define QUOTED_FIRST   '\x03'  /* prefix: the word's first character was quoted (no ~) */
#define ESCAPED_DOLLAR '\x04'  /* a \$ inside double quotes: a literal $ */

/* ---- syntax tree ---- */

typedef enum {
    R_IN,       /* [n]<word    */
    R_OUT,      /* [n]>word, [n]>|word */
    R_APPEND,   /* [n]>>word   */
    R_RDWR,     /* [n]<>word   */
    R_DUP       /* [n]<&word, [n]>&word ("-" closes n) */
} RedirKind;

typedef struct Redir {
    int           fd;      /* the fd being redirected (0-9) */
    RedirKind     kind;
    char         *target;  /* word, still marked; expanded when applied */
    struct Redir *next;
} Redir;

typedef enum {
    N_SIMPLE,     /* assignments, words, redirections */
    N_PIPELINE,   /* cmd | cmd ... (optionally !-negated) */
    N_AND,        /* left && right */
    N_OR,         /* left || right */
    N_SEQ,        /* left ; right */
    N_BACKGROUND, /* body & */
    N_SUBSHELL,   /* ( body ) */
    N_GROUP,      /* { body; } */
    N_IF,
    N_WHILE,
    N_UNTIL,
    N_FOR,
    N_CASE,
    N_FUNCDEF     /* name() body */
} NodeKind;

typedef struct Node Node;

typedef struct CaseItem {
    char           **patterns;  /* marked words */
    int              npatterns;
    Node            *body;      /* may be NULL (empty clause) */
    struct CaseItem *next;
} CaseItem;

struct Node {
    NodeKind kind;
    int      refs;     /* function bodies are shared with the function table */
    Redir   *redirs;   /* N_SIMPLE and compound commands */
    char    *text;     /* source text of a pipeline / background job, for `jobs` */
    union {
        struct { char **words; int nwords; char **assigns; int nassigns; } simple;
        struct { Node **cmds; int ncmds; int bang; } pipe;
        struct { Node *left, *right; } bin;                    /* AND, OR, SEQ */
        struct { Node *body; } sub;                            /* BACKGROUND, SUBSHELL, GROUP */
        struct { Node *cond, *then_part, *else_part; } ifs;    /* elif = nested IF in else_part */
        struct { Node *cond, *body; } loop;                    /* WHILE, UNTIL */
        struct { char *var; char **words; int nwords; int has_in; Node *body; } forl;
        struct { char *subject; CaseItem *items; } cases;
        struct { char *name; Node *body; } func;
    } u;
};

/* Takes / drops a reference; the last drop frees the whole subtree. */
Node *node_ref(Node *n);
void  node_unref(Node *n);

/* ---- input and lexer ---- */

/* Supplies the next line of input (heap, caller frees, normally ending
 * in '\n'), or NULL at end of input. `continuation` is 0 when a new
 * command starts (show PS1) and 1 when a command needs more lines
 * (show PS2). */
typedef char *(*LineSource)(void *ctx, int continuation);

typedef struct Lexer Lexer;

/* A lexer over lines from `source` (may be NULL: then `text` is all the
 * input there is). `name` prefixes error messages ("script.sh"). */
Lexer *lexer_new(const char *text, LineSource source, void *ctx, const char *name);
void   lexer_free(Lexer *lx);

/* ---- parser ---- */

typedef enum {
    PARSE_OK,     /* *out is a command to run (or NULL for an empty line) */
    PARSE_EOF,    /* no more input */
    PARSE_ERROR   /* syntax error, already reported; rest of the line dropped */
} ParseResult;

/* Parses the next complete command from `lx`. */
ParseResult parse_next(Lexer *lx, Node **out);

/* Drops whatever input is buffered (e.g. after Ctrl+C at the prompt). */
void lex_discard_input(Lexer *lx);

/* ---- executor (exec.c) and functions (functions.c) ---- */

/* Runs a syntax tree in this shell and returns its exit status. */
int exec_node(Node *n);

/* Parses and runs every command from `lx`. Returns 0 at end of input,
 * -1 after a syntax error (status 2), -2 after Ctrl+C stopped a command. */
int run_lexer(Lexer *lx);

/* Defines (or redefines) function `name`, taking a reference to body. */
void function_define(const char *name, Node *body);

#endif /* ACSH_SYNTAX_H */
