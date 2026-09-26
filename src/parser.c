#define _POSIX_C_SOURCE 200809L  /* needed for strdup/strndup under -std=c11 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "acsh.h"

/* Small, POSIX-minded tokenizer. Splits on whitespace and treats
 * |, <, >, >>, & as their own tokens even without surrounding spaces
 * (e.g. "ls|grep" tokenizes the same as "ls | grep").
 *
 * Quoting support:
 *   'literal text'   -- everything inside is taken as-is, no escapes
 *                        recognized, and (crucially) $ is NOT expanded
 *                        inside single quotes -- true POSIX behaviour.
 *   "literal text"   -- everything inside is taken as-is, EXCEPT a
 *                        backslash before " or \ or $ is still special,
 *                        and $VAR / ${VAR} ARE expanded inside double
 *                        quotes, matching POSIX.
 *   \x               -- outside any quotes, escapes the single
 *                        character x, so e.g.  ls\ -l  is one token
 *                        "ls -l", and  echo \| stays a literal pipe
 *                        char instead of being parsed as a pipe.
 *
 * A word may mix quoted and unquoted parts, e.g.  echo hello" "world
 * becomes one token "hello world".
 *
 * Implementation note: tokenizing and $VAR expansion are kept as two
 * separate passes (like a real shell's word-expansion stage). To let
 * the expansion pass know which '$' characters came from inside
 * single quotes (and must NOT be expanded) vs everywhere else (which
 * MUST be expanded), read_word() wraps single-quoted content in the
 * sentinel bytes SQ_START/SQ_END. env_expand() (env.c) recognizes
 * these, skips expanding '$' between them, and strips the sentinels
 * back out before returning the final string.
 *
 * Tilde expansion (~ -> $HOME) only ever applies when the ~ is the
 * very first character of a word AND that character was not quoted
 * (single or double). To let env_expand() know this without a full
 * per-character quote-tracking scheme, read_word() prefixes the
 * output with the sentinel byte QUOTED_FIRST whenever the word's
 * first character came from inside ANY quote type -- env_expand()
 * checks for this one flag before attempting tilde expansion, then
 * strips it. */

#define SQ_START     '\x01'
#define SQ_END       '\x02'
#define QUOTED_FIRST '\x03'

#define MAX_TOKENS 256

/* Reads one "word" token starting at *pp (which may begin mid-word,
 * with quotes and unquoted segments mixed). Appends the decoded
 * characters into buf. Returns 0 on success, -1 on an unterminated
 * quote (error already printed). Advances *pp past the word. */
static int read_word(char **pp, char *buf, size_t bufsize) {
    char *p = *pp;
    size_t len = 0;
    int is_first_char = 1;
    int first_char_quoted = 0;

    while (*p != '\0') {
        if (*p == ' ' || *p == '\t' ||
            *p == '|' || *p == '<' || *p == '>' || *p == '&') {
            break; /* end of this word */
            }

            if (*p == '\'') {
                /* single-quoted: copy verbatim until the closing quote,
                 * wrapped in sentinels so env_expand() skips it */
                if (is_first_char) { first_char_quoted = 1; is_first_char = 0; }
                p++;
                if (len + 1 >= bufsize) goto too_long;
                buf[len++] = SQ_START;
                while (*p != '\0' && *p != '\'') {
                    if (len + 1 >= bufsize) goto too_long;
                    buf[len++] = *p++;
                }
                if (*p != '\'') {
                    fprintf(stderr, "acsh: syntax error: unterminated '\n");
                    return -1;
                }
                if (len + 1 >= bufsize) goto too_long;
                buf[len++] = SQ_END;
                p++; /* skip closing quote */
                continue;
            }

            if (*p == '"') {
                /* double-quoted: copy verbatim, but \" \\ \$ stay escapes;
                 * $VAR expansion still applies later, so no sentinels here */
                if (is_first_char) { first_char_quoted = 1; is_first_char = 0; }
                p++;
                while (*p != '\0' && *p != '"') {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
                        if (len + 1 >= bufsize) goto too_long;
                        buf[len++] = p[1];
                        p += 2;
                    } else {
                        if (len + 1 >= bufsize) goto too_long;
                        buf[len++] = *p++;
                    }
                }
                if (*p != '"') {
                    fprintf(stderr, "acsh: syntax error: unterminated \"\n");
                    return -1;
                }
                p++; /* skip closing quote */
                continue;
            }

            if (*p == '\\' && p[1] != '\0') {
                /* backslash outside quotes escapes exactly the next char */
                is_first_char = 0;
                if (len + 1 >= bufsize) goto too_long;
                buf[len++] = p[1];
                p += 2;
                continue;
            }

            if (len + 1 >= bufsize) goto too_long;
            buf[len++] = *p++;
        is_first_char = 0;
    }

    buf[len] = '\0';
    *pp = p;

    if (first_char_quoted) {
        /* shift everything right by one and prefix the marker byte,
         * so env_expand() can check buf[0] == QUOTED_FIRST cheaply */
        if (len + 2 >= bufsize) goto too_long;
        memmove(buf + 1, buf, len + 1); /* +1 to move the NUL too */
        buf[0] = QUOTED_FIRST;
    }

    return 0;

    too_long:
    fprintf(stderr, "acsh: word too long\n");
    return -1;
}

static int tokenize(char *line, char *tokens[], int max_tokens) {
    int n = 0;
    char *p = line;
    char wordbuf[ACSH_MAX_LINE];

    while (*p != '\0') {
        /* skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        if (n >= max_tokens) {
            fprintf(stderr, "acsh: too many tokens\n");
            return -1;
        }

        if (*p == '#') {
            /* unquoted # starts a comment: everything after it to end
             * of line is ignored, matching POSIX shell comment rules.
             * We only ever reach this point between words, never mid-
             * quote (read_word consumes entire quoted regions itself),
             * so this check is safe here. */
            break;
        }

        if (*p == '|' || *p == '<' || *p == '&' ) {
            tokens[n] = strndup(p, 1);
            n++;
            p++;
            continue;
        }

        if (*p == '>') {
            if (*(p + 1) == '>') {
                tokens[n] = strndup(p, 2);   /* ">>" */
                n++;
                p += 2;
            } else {
                tokens[n] = strndup(p, 1);   /* ">" */
                n++;
                p += 1;
            }
            continue;
        }

        /* regular word: may contain quoted and unquoted segments */
        if (read_word(&p, wordbuf, sizeof(wordbuf)) != 0) {
            return -1;
        }
        tokens[n] = strdup(wordbuf);
        n++;
    }

    return n;
}

static void free_tokens(char *tokens[], int n) {
    for (int i = 0; i < n; i++) {
        free(tokens[i]);
    }
}

int parse_line(char *line, Pipeline *pl) {
    strncpy(pl->raw_line, line, ACSH_MAX_LINE - 1);
    pl->raw_line[ACSH_MAX_LINE - 1] = '\0';

    char *tokens[MAX_TOKENS];
    int ntok = tokenize(line, tokens, MAX_TOKENS);
    if (ntok < 0) {
        return -1;
    }
    if (ntok == 0) {
        pl->num_cmds = 0;
        return 0;
    }

    int ci = 0;                  /* current command index in pl->cmds */
    Command *cur = &pl->cmds[ci];
    cur->argc = 0;

    for (int i = 0; i < ntok; i++) {
        char *tok = tokens[i];

        if (strcmp(tok, "|") == 0) {
            cur->argv[cur->argc] = NULL;
            ci++;
            if (ci >= ACSH_MAX_CMDS) {
                fprintf(stderr, "acsh: too many piped commands\n");
                free_tokens(tokens, ntok);
                return -1;
            }
            cur = &pl->cmds[ci];
            cur->argc = 0;
            continue;
        }

        if (strcmp(tok, "<") == 0) {
            i++;
            if (i >= ntok) {
                fprintf(stderr, "acsh: syntax error: expected filename after <\n");
                free_tokens(tokens, ntok);
                return -1;
            }
            cur->infile = env_expand(tokens[i]);
            continue;
        }

        if (strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0) {
            int append = (strcmp(tok, ">>") == 0);
            i++;
            if (i >= ntok) {
                fprintf(stderr, "acsh: syntax error: expected filename after %s\n", tok);
                free_tokens(tokens, ntok);
                return -1;
            }
            cur->outfile = env_expand(tokens[i]);
            cur->append = append;
            continue;
        }

        if (strcmp(tok, "&") == 0) {
            pl->background = 1;
            continue;
        }

        /* regular argument word: env_expand ($VAR, ~) happens first,
         * then glob_expand (star, ?, [) against the result -- this
         * order matters, e.g. "$DIR" followed by "star.c" must expand
         * DIR before globbing. A single word can expand into MULTIPLE
         * argv entries here (e.g. star.c -> main.c parser.c ...). */
        {
            char *var_expanded = env_expand(tok);
            if (var_expanded == NULL) {
                fprintf(stderr, "acsh: out of memory\n");
                free_tokens(tokens, ntok);
                return -1;
            }

            char *glob_results[ACSH_MAX_ARGS];
            int nglob = glob_expand(var_expanded, glob_results, ACSH_MAX_ARGS - cur->argc - 1);
            free(var_expanded);

            for (int g = 0; g < nglob; g++) {
                if (cur->argc >= ACSH_MAX_ARGS - 1) {
                    fprintf(stderr, "acsh: too many arguments\n");
                    free_tokens(tokens, ntok);
                    return -1;
                }
                cur->argv[cur->argc] = glob_results[g];
                cur->argc++;
            }
        }
    }

    cur->argv[cur->argc] = NULL;
    pl->num_cmds = ci + 1;

    free_tokens(tokens, ntok);
    return 0;
}

/* Lightweight quote-aware scan used ONLY to find the split points for
 * &&, ||, and ; -- this does NOT do full tokenizing (no word
 * decoding, no escape handling); it just needs to track whether we're
 * currently inside a ' or " so that e.g.  echo "a && b"  is NOT split
 * into two segments. Full decoding (and $VAR/$?/alias expansion)
 * happens later, per-segment, just before each one executes -- see
 * execute_chain() in executor.c for why that ordering matters. */
int parse_chain(const char *line, Chain *ch) {
    ch->num_segments = 0;

    const char *seg_start = line;
    const char *p = line;
    char in_quote = '\0'; /* '\0' = not in a quote, else the quote char */

    while (1) {
        int is_end_of_line = (*p == '\0');
        int split_here = 0;
        ChainOp op = CHAIN_END;
        int consume = 0; /* how many chars the operator itself takes */

        if (!is_end_of_line && in_quote == '\0') {
            if (*p == '\'' || *p == '"') {
                in_quote = *p;
            } else if (*p == '&' && p[1] == '&') {
                split_here = 1; op = CHAIN_AND; consume = 2;
            } else if (*p == '|' && p[1] == '|') {
                split_here = 1; op = CHAIN_OR; consume = 2;
            } else if (*p == ';') {
                split_here = 1; op = CHAIN_SEQ; consume = 1;
            }
        } else if (!is_end_of_line && in_quote != '\0') {
            if (*p == in_quote) {
                in_quote = '\0';
            }
            /* NOTE: this simple scan doesn't special-case backslash-
             * escaped quotes inside "..." (e.g. "a \" && b") -- an
             * acceptable simplification for a mini-project; real
             * shells handle this via full tokenizing at this stage. */
        }

        if (split_here || is_end_of_line) {
            size_t seg_len = (size_t)(p - seg_start);

            /* trim leading/trailing whitespace from the segment so an
             * empty/whitespace-only segment (e.g. leading ";;" or
             * trailing "&&") is correctly detected as empty below */
            const char *trim_start = seg_start;
            const char *trim_end = seg_start + seg_len;
            while (trim_start < trim_end && (*trim_start == ' ' || *trim_start == '\t')) trim_start++;
            while (trim_end > trim_start && (trim_end[-1] == ' ' || trim_end[-1] == '\t')) trim_end--;
            size_t trimmed_len = (size_t)(trim_end - trim_start);

            if (trimmed_len > 0) {
                if (ch->num_segments >= ACSH_MAX_CHAIN) {
                    fprintf(stderr, "acsh: too many chained commands\n");
                    return -1;
                }
                if (trimmed_len >= ACSH_MAX_LINE) {
                    fprintf(stderr, "acsh: chained command too long\n");
                    return -1;
                }
                memcpy(ch->segments[ch->num_segments], trim_start, trimmed_len);
                ch->segments[ch->num_segments][trimmed_len] = '\0';
                ch->ops[ch->num_segments] = is_end_of_line ? CHAIN_END : op;
                ch->num_segments++;
            }

            if (is_end_of_line) {
                break;
            }

            p += consume;
            seg_start = p;
            continue;
        }

        p++;
    }

    if (in_quote != '\0') {
        fprintf(stderr, "acsh: syntax error: unterminated %c\n", in_quote);
        return -1;
    }

    return 0;
}
