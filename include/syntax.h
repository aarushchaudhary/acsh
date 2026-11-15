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
#define SQ_START '\x01'  /* single-quoted or escaped text: taken literally */
#define SQ_END   '\x02'
#define DQ_START '\x05'  /* double-quoted text: expanded, never split or globbed */
#define DQ_END   '\x06'

/* ---- syntax tree ---- */

typedef enum {
    R_IN,       /* [n]<word    */
    R_OUT,      /* [n]>word */
    R_CLOBBER,  /* [n]>|word: overwrites even under set -C */
    R_APPEND,   /* [n]>>word   */
    R_RDWR,     /* [n]<>word   */
    R_DUP,      /* [n]<&word, [n]>&word ("-" closes n) */
    R_HEREDOC   /* [n]<<word, [n]<<-word: target holds the body */
} RedirKind;

typedef struct Redir {
    int           fd;      /* the fd being redirected (0-9) */
    RedirKind     kind;
    char         *target;  /* word, still marked; expanded when applied (for
                            * R_HEREDOC: the body, marked like a word) */
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
    int              fallthrough; /* ended by ;& : the next body runs too */
    struct CaseItem *next;
} CaseItem;

struct Node {
    NodeKind kind;
    int      refs;     /* function bodies are shared with the function table */
    int      lineno;   /* source line where the command starts ($LINENO) */
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

/* Decodes raw shell text (the inside of ${x:-word} or $((...))) into
 * marked form, as if it were one word; with dquoted, as if it stood
 * inside double quotes. Returns a heap string, or NULL on a quoting
 * error (reported). */
char *lex_mark(const char *raw, int dquoted);
void   lexer_free(Lexer *lx);

/* Finds the ')' that closes a $( ... ): fed every character outside
 * quotes (after the opening parenthesis), it counts parentheses but
 * skips the ')' ending a case pattern (`case x in a) ...`), which has
 * no '('. Start from a zeroed ParenScan with depth 1. */
typedef struct {
    int  depth;
    int  case_at[16], case_state[16], ncase; /* open `case`s: depth, 0 word 1 subject 2 in */
    char word[8];
    int  wlen;
    int  cmd_start;  /* the next word is in command position */
} ParenScan;
void paren_scan_init(ParenScan *ps);
int  paren_scan(ParenScan *ps, int c); /* 1: c is the closing ')' */

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
