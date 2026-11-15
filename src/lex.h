/* Private interface between lexer.c and parse.c. */
#ifndef ACSH_LEX_H
#define ACSH_LEX_H

#include <stddef.h>
#include "syntax.h"

typedef enum {
    T_WORD, T_IO_NUMBER, T_NEWLINE, T_EOF,
    T_SEMI, T_AMP, T_PIPE, T_AND_IF, T_OR_IF, T_DSEMI, T_LPAREN, T_RPAREN,
    T_LESS, T_GREAT, T_DGREAT, T_LESSAND, T_GREATAND, T_LESSGREAT, T_CLOBBER,
    T_DLESS, T_DLESSDASH, T_ANDGREAT, T_ANDDGREAT, T_SEMIAND
} TokType;

typedef struct {
    TokType type;
    char   *text;    /* T_WORD: marked word (heap); T_IO_NUMBER: the digits */
    int     quoted;  /* the word contained quoting or escapes */
    size_t  start;   /* source position (buffer offset) of the token */
    size_t  end;
} Token;

struct Lexer {
    char      *buf;          /* input read so far for the current command */
    size_t     len, cap, pos;
    LineSource source;       /* NULL: buf is all the input there is */
    void      *ctx;
    int        at_eof;
    int        new_command;  /* next line fetched starts a new command (PS1) */
    size_t     alias_end;    /* end of the text the latest alias expansion inserted */
    int        alias_blank;  /* that alias ended in a blank: the word after it is checked too */
    struct { char *name; size_t end; } active[16]; /* aliases whose text is still being read */
    int        nactive;
    int        base_line;    /* lines already dropped from the front of buf */
    char      *name;         /* for error messages; NULL at the prompt */
    struct PendingDoc *docs; /* here-documents whose bodies start after the next newline */
};

/* Registers a here-document: when the lexer reaches the end of the
 * current line it reads the body up to a line equal to `delim` and
 * stores it in r->target. quoted: the delimiter was quoted, so the
 * body is taken literally. strip_tabs: <<- (leading tabs removed). */
void lex_add_heredoc(Lexer *lx, Redir *r, const char *delim, int quoted, int strip_tabs);

/* Reads the next token into *t. Returns 0, or -1 after reporting an
 * error (unterminated quote or substitution). */
int  lex_next(Lexer *lx, Token *t);

/* If *t is an unquoted word that names an alias (and is not itself text
 * produced by an alias), replaces it in the input with the alias value
 * and returns 1: the caller drops *t and reads the next token again. */
int  lex_expand_alias(Lexer *lx, const Token *t);

/* Starts a new complete command: drops consumed input. */
void lex_begin_command(Lexer *lx);

/* After a syntax error: drops the rest of the current input line(s). */
void lex_discard(Lexer *lx);

/* Prints "acsh: [name: line N: ]syntax error: <msg>" for position pos,
 * plus "  acsh: usage:  <usage>" when usage is not NULL. */
void lex_syntax_error(Lexer *lx, size_t pos, const char *usage, const char *fmt, ...);

/* The source line of buffer position pos (first line 1; -c: 0, as ash). */
int  lex_line_at(Lexer *lx, size_t pos);

/* Copies source text [start, end) (heap). */
char *lex_source(Lexer *lx, size_t start, size_t end);

#endif
