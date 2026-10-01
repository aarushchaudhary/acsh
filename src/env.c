#define _POSIX_C_SOURCE 200809L  /* strdup, strndup under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "acsh.h"

/* Must match the sentinel bytes parser.c wraps single-quoted content
 * in. They mark regions where '$' must NOT be expanded (true POSIX
 * single-quote behaviour). See parser.c's tokenizer comment for why
 * this two-pass design (tokenize, then expand) is used. */
#define SQ_START     '\x01'
#define SQ_END       '\x02'
#define QUOTED_FIRST '\x03'

/* Returns 1 if c can appear in a shell variable name ([A-Za-z0-9_]). */
static int is_var_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* Holds the exit status of the most recently completed pipeline, so
 * $? can expand to it. Set by env_set_last_status(), called from
 * execute_chain() (executor.c) after each pipeline finishes -- this
 * is the same value POSIX shells expose as $?. */
static int last_exit_status = 0;

void env_set_last_status(int status) {
    last_exit_status = status;
}

/* Getter counterpart, used by control_flow.c to read back a
 * condition/body chain's exit status without re-parsing "$?" text. */
int env_get_last_status(void) {
    return last_exit_status;
}

/* Positional parameters ($1, $2, ..., $#, $@) for the CURRENTLY
 * EXECUTING function call. Kept as a small stack (not just one global
 * set) so a function calling another function -- including itself,
 * recursively -- gets its own $1.../$# while it runs, and the outer
 * call's parameters are correctly restored once the inner call
 * returns. functions.c pushes a new frame before running a function's
 * body and pops it afterward; env_expand() below only ever reads the
 * TOP of this stack, i.e. whichever function call is innermost right
 * now. Outside of any function call, the stack is empty and $1/$@/$#
 * all expand to nothing/zero, matching a shell's top-level state. */
static char *positional_stack[ACSH_MAX_CALL_DEPTH][ACSH_MAX_POSITIONAL_PARAMS];
static int   positional_count_stack[ACSH_MAX_CALL_DEPTH];
static int   positional_stack_depth = 0;

void env_push_positional_params(int argc, char *argv[]) {
    if (positional_stack_depth >= ACSH_MAX_CALL_DEPTH) {
        /* silently cap recursion depth rather than corrupt the stack;
         * a function this deep is almost certainly an infinite
         * recursion bug in the script, not a legitimate use case */
        return;
    }
    int n = argc;
    if (n > ACSH_MAX_POSITIONAL_PARAMS) {
        n = ACSH_MAX_POSITIONAL_PARAMS;
    }
    for (int i = 0; i < n; i++) {
        positional_stack[positional_stack_depth][i] = strdup(argv[i]);
    }
    positional_count_stack[positional_stack_depth] = n;
    positional_stack_depth++;
}

/* Reports how many function calls are currently nested (i.e. how many
 * frames env_push_positional_params() has pushed without a matching
 * pop yet). function_call() (functions.c) checks this BEFORE
 * recursing further, so a runaway recursive function gets a clean
 * "recursion too deep" error instead of silently exhausting the
 * real C call stack and segfaulting -- env_push_positional_params()'s
 * own cap only protects ITS storage array, not the unbounded C
 * recursion in function_call()/run_statements() that was still
 * happening past that point before this check was added. */
int env_get_call_depth(void) {
    return positional_stack_depth;
}

void env_pop_positional_params(void) {
    if (positional_stack_depth <= 0) {
        return;
    }
    positional_stack_depth--;
    int n = positional_count_stack[positional_stack_depth];
    for (int i = 0; i < n; i++) {
        free(positional_stack[positional_stack_depth][i]);
    }
}

char *env_expand(const char *word) {
    /* Strip the QUOTED_FIRST marker (if present) and remember whether
     * it was there -- it tells us the word's first character came
     * from inside a quote, so tilde expansion must NOT apply (e.g.
     * echo "~" or echo '~' must print a literal tilde). */
    int first_char_was_quoted = 0;
    if (word[0] == QUOTED_FIRST) {
        first_char_was_quoted = 1;
        word++;
    }

    /* Tilde expansion: POSIX expands a leading ~ to $HOME, but only
     * when it's the very first character of the word and that
     * character was not quoted. We only handle the plain `~` and
     * `~/rest` forms here; `~username` (another user's home dir) is
     * out of scope. */
    const char *src = word;
    char *tilde_expanded = NULL;

    if (!first_char_was_quoted &&
        word[0] == '~' && (word[1] == '/' || word[1] == '\0')) {
        const char *home = getenv("HOME");
    if (home != NULL) {
        size_t total = strlen(home) + strlen(word + 1) + 1;
        tilde_expanded = malloc(total);
        if (tilde_expanded != NULL) {
            snprintf(tilde_expanded, total, "%s%s", home, word + 1);
            src = tilde_expanded;
        }
    }
        }

        size_t cap = strlen(src) * 2 + 32; /* generous starting buffer */
        char *out = malloc(cap);
        size_t len = 0;
        const char *p = src;
        int in_single_quotes = 0;

        if (out == NULL) {
            free(tilde_expanded);
            return NULL;
        }

        while (*p != '\0') {
            if (*p == SQ_START) {
                in_single_quotes = 1;
                p++;
                continue;
            }
            if (*p == SQ_END) {
                in_single_quotes = 0;
                p++;
                continue;
            }

            if (*p == '`' && !in_single_quotes) {
                /* `cmd` -- old-style command substitution: everything up
                 * to the next backtick is the inner command */
                const char *inner_start = p + 1;
                const char *inner_end = strchr(inner_start, '`');
                if (inner_end != NULL) {
                    size_t inner_len = (size_t)(inner_end - inner_start);
                    char *inner = malloc(inner_len + 1);
                    if (inner != NULL) {
                        memcpy(inner, inner_start, inner_len);
                        inner[inner_len] = '\0';
                        char *result = command_substitute(inner);
                        free(inner);
                        if (result != NULL) {
                            size_t vlen = strlen(result);
                            while (len + vlen + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
                            memcpy(out + len, result, vlen);
                            len += vlen;
                            free(result);
                        }
                    }
                    p = inner_end + 1;
                    continue;
                }
                /* no closing backtick: fall through and copy it literally */
            }

            if (*p != '$' || in_single_quotes) {
                if (len + 1 >= cap) {
                    cap *= 2;
                    out = realloc(out, cap);
                }
                out[len++] = *p++;
                continue;
            }

            /* *p == '$' and we are NOT inside single quotes */

            if (*(p + 1) == '(') {
                /* $(cmd) -- find the matching ')' by depth-counting, so a
                 * nested $(...) inside the command is included whole, and
                 * skipping over quoted text so a ')' inside quotes is not
                 * mistaken for the closer. */
                const char *inner_start = p + 2;
                const char *q = inner_start;
                int depth = 1;
                char in_q = '\0';
                while (*q != '\0') {
                    if (in_q != '\0') {
                        if (*q == in_q) in_q = '\0';
                    } else if (*q == '\'' || *q == '"') {
                        in_q = *q;
                    } else if (*q == '(') {
                        depth++;
                    } else if (*q == ')') {
                        depth--;
                        if (depth == 0) break;
                    }
                    q++;
                }
                if (*q == ')') {
                    size_t inner_len = (size_t)(q - inner_start);
                    char *inner = malloc(inner_len + 1);
                    if (inner != NULL) {
                        memcpy(inner, inner_start, inner_len);
                        inner[inner_len] = '\0';
                        char *result = command_substitute(inner);
                        free(inner);
                        if (result != NULL) {
                            size_t vlen = strlen(result);
                            while (len + vlen + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
                            memcpy(out + len, result, vlen);
                            len += vlen;
                            free(result);
                        }
                    }
                    p = q + 1;
                    continue;
                }
                /* unterminated: the tokenizer already reported this, so
                 * just fall through and treat the '$' literally */
            }
            if (*(p + 1) >= '1' && *(p + 1) <= '9') {
                /* $1..$9 -- positional parameter from the innermost
                 * currently-executing function call (see the positional
                 * parameter stack above). Outside any function call, or
                 * for an index beyond how many arguments were passed,
                 * this expands to empty string, matching POSIX (an unset
                 * parameter is simply empty, not an error). */
                int idx = *(p + 1) - '1'; /* $1 -> index 0 */
                p += 2;
                if (positional_stack_depth > 0) {
                    int top = positional_stack_depth - 1;
                    if (idx < positional_count_stack[top]) {
                        const char *val = positional_stack[top][idx];
                        size_t vlen = strlen(val);
                        while (len + vlen + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
                        memcpy(out + len, val, vlen);
                        len += vlen;
                    }
                }
                continue;
            }

            if (*(p + 1) == '#') {
                /* $# -- number of positional parameters passed to the
                 * innermost currently-executing function call. */
                p += 2;
                int count = (positional_stack_depth > 0)
                ? positional_count_stack[positional_stack_depth - 1] : 0;
                char count_str[16];
                int count_len = snprintf(count_str, sizeof(count_str), "%d", count);
                if (count_len > 0) {
                    size_t vlen = (size_t)count_len;
                    while (len + vlen + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
                    memcpy(out + len, count_str, vlen);
                    len += vlen;
                }
                continue;
            }

            if (*(p + 1) == '@') {
                /* $@ -- all positional parameters, space-separated. (A
                 * simplification vs full POSIX, where "$@" inside double
                 * quotes expands to separate individually-quoted words;
                 * acsh always joins them with a single space here.) */
                p += 2;
                if (positional_stack_depth > 0) {
                    int top = positional_stack_depth - 1;
                    int count = positional_count_stack[top];
                    for (int i = 0; i < count; i++) {
                        if (i > 0) {
                            if (len + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
                            out[len++] = ' ';
                        }
                        const char *val = positional_stack[top][i];
                        size_t vlen = strlen(val);
                        while (len + vlen + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
                        memcpy(out + len, val, vlen);
                        len += vlen;
                    }
                }
                continue;
            }

            if (*(p + 1) == '?') {
                /* $? -- exit status of the last completed pipeline. Kept
                 * as a special case here rather than a real getenv() var,
                 * since it changes after every command and isn't part of
                 * the process environment -- exactly how POSIX shells
                 * treat it (a shell-internal parameter, not a real
                 * variable). */
                p += 2; /* skip '$' and '?' */
                char code_str[16];
                int code_len = snprintf(code_str, sizeof(code_str), "%d", last_exit_status);
                if (code_len > 0) {
                    size_t vlen = (size_t)code_len;
                    while (len + vlen + 1 >= cap) {
                        cap *= 2;
                        out = realloc(out, cap);
                    }
                    memcpy(out + len, code_str, vlen);
                    len += vlen;
                }
                continue;
            }

            const char *name_start;
            size_t name_len;
            p++; /* skip '$' */

            if (*p == '{') {
                p++;
                name_start = p;
                while (*p != '\0' && *p != '}') p++;
                name_len = (size_t)(p - name_start);
                if (*p == '}') p++; /* skip closing brace; if missing, be lenient */
            } else if (is_var_char(*p) || *p == '\0') {
                name_start = p;
                while (is_var_char(*p)) p++;
                name_len = (size_t)(p - name_start);
            } else {
                /* '$' not followed by a valid name (e.g. "$" alone, or
                 * "$5", or "$@") -- just emit the '$' literally for now.
                 * Positional/special parameters are out of scope. */
                if (len + 1 >= cap) {
                    cap *= 2;
                    out = realloc(out, cap);
                }
                out[len++] = '$';
                continue;
            }

            if (name_len == 0) {
                /* "${}" or a bare trailing "$" with nothing valid after it */
                if (len + 1 >= cap) {
                    cap *= 2;
                    out = realloc(out, cap);
                }
                out[len++] = '$';
                continue;
            }

            char name[256];
            size_t copy_len = name_len < sizeof(name) - 1 ? name_len : sizeof(name) - 1;
            memcpy(name, name_start, copy_len);
            name[copy_len] = '\0';

            const char *value = getenv(name);
            if (value != NULL) {
                size_t vlen = strlen(value);
                while (len + vlen + 1 >= cap) {
                    cap *= 2;
                    out = realloc(out, cap);
                }
                memcpy(out + len, value, vlen);
                len += vlen;
            }
            /* if the variable isn't set, POSIX expands it to empty string,
             * which is exactly what happens here by doing nothing */
        }

        out[len] = '\0';
        free(tilde_expanded);
        return out;
}
