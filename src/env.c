#define _POSIX_C_SOURCE 200809L  /* strdup, strndup under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "acsh.h"

#include "syntax.h"  /* the marker bytes inside words */

/* Returns 1 if c can appear in a shell variable name ([A-Za-z0-9_]). */
static int is_var_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* $?: the exit status of the most recently completed pipeline or
 * compound command, set by the executor (exec.c). */
static int last_exit_status = 0;

void env_set_last_status(int status) {
    last_exit_status = status;
}

/* Getter counterpart, used by the executor and builtins. */
int env_get_last_status(void) {
    return last_exit_status;
}

/* One frame per running function call: its positional parameters
 * ($1, $2, ..., $#, $@) and the variables it declared `local`, with the
 * values they had before, restored when the call returns. Only the
 * innermost frame is visible; outside any function there is none, and
 * $1/$@/$# are empty. Everything is allocated per call, so neither the
 * number of arguments nor of locals is limited. */
typedef struct {
    char *name;
    char *prior_value;   /* NULL if the name was unset before */
} LocalVar;

typedef struct {
    char    **params;
    int       nparams;
    LocalVar *locals;
    int       nlocals;
} Frame;

static Frame frames[ACSH_MAX_CALL_DEPTH];
static int   depth = 0;

static char **copy_params(int argc, char *argv[]) {
    char **params = malloc((size_t)(argc > 0 ? argc : 1) * sizeof(char *));
    if (params == NULL) {
        fprintf(stderr, "acsh: out of memory\n");
        exit(2);
    }
    for (int i = 0; i < argc; i++) {
        params[i] = strdup(argv[i]);
    }
    return params;
}

static void free_params(Frame *f) {
    for (int i = 0; i < f->nparams; i++) {
        free(f->params[i]);
    }
    free(f->params);
    f->params = NULL;
    f->nparams = 0;
}

void env_push_positional_params(int argc, char *argv[]) {
    if (depth >= ACSH_MAX_CALL_DEPTH) {
        return; /* function_call() refuses to recurse this deep first */
    }
    Frame *f = &frames[depth++];
    f->params = copy_params(argc, argv);
    f->nparams = argc;
    f->locals = NULL;
    f->nlocals = 0;
}

void env_pop_positional_params(void) {
    if (depth > 0) {
        free_params(&frames[--depth]);
    }
}

/* Starts the (empty) locals of the call just pushed. */
void env_push_locals_frame(void) {
    if (depth > 0) {
        frames[depth - 1].locals = NULL;
        frames[depth - 1].nlocals = 0;
    }
}

/* `local NAME[=value]`: remembers NAME's current value (only the first
 * time this call declares it, so the true outer value comes back) and
 * sets it, or unsets it for a bare `local x`. Returns 1 outside a
 * function, as real shells do. */
int env_declare_local(const char *name, const char *value) {
    if (depth <= 0) {
        return 1;
    }
    Frame *f = &frames[depth - 1];
    int known = 0;
    for (int i = 0; i < f->nlocals; i++) {
        if (strcmp(f->locals[i].name, name) == 0) {
            known = 1;
            break;
        }
    }
    if (!known) {
        LocalVar *bigger = realloc(f->locals, (size_t)(f->nlocals + 1) * sizeof(LocalVar));
        if (bigger == NULL) {
            return 1;
        }
        f->locals = bigger;
        const char *existing = getenv(name);
        f->locals[f->nlocals].name = strdup(name);
        f->locals[f->nlocals].prior_value = existing != NULL ? strdup(existing) : NULL;
        f->nlocals++;
    }
    if (value != NULL) {
        setenv(name, value, 1);
    } else if (!known) {
        unsetenv(name);
    }
    return 0;
}

/* Restores every variable the returning call declared local. Called
 * before env_pop_positional_params(). */
void env_pop_locals_frame(void) {
    if (depth <= 0) {
        return;
    }
    Frame *f = &frames[depth - 1];
    for (int i = 0; i < f->nlocals; i++) {
        LocalVar *v = &f->locals[i];
        if (v->prior_value != NULL) {
            setenv(v->name, v->prior_value, 1);
        } else {
            unsetenv(v->name);
        }
        free(v->name);
        free(v->prior_value);
    }
    free(f->locals);
    f->locals = NULL;
    f->nlocals = 0;
}

/* `set -- ARG...`: replaces the current call's parameters. Returns 1
 * outside a function (acsh has no top-level parameters yet). */
int env_set_positional_params(int argc, char *argv[]) {
    if (depth <= 0) {
        return 1;
    }
    Frame *f = &frames[depth - 1];
    char **params = copy_params(argc, argv); /* before freeing: argv may be $@ itself */
    free_params(f);
    f->params = params;
    f->nparams = argc;
    return 0;
}

/* `shift [n]`. Returns 1 if n exceeds the parameters there are, or
 * outside a function. */
int env_shift_positional_params(int n) {
    if (depth <= 0) {
        return 1;
    }
    Frame *f = &frames[depth - 1];
    if (n < 0 || n > f->nparams) {
        return 1;
    }
    for (int i = 0; i < n; i++) {
        free(f->params[i]);
    }
    memmove(f->params, f->params + n, (size_t)(f->nparams - n) * sizeof(char *));
    f->nparams -= n;
    return 0;
}

int env_positional_count(void) {
    return depth > 0 ? frames[depth - 1].nparams : 0;
}

const char *env_positional(int i) {
    if (i < 1 || i > env_positional_count()) {
        return "";
    }
    return frames[depth - 1].params[i - 1];
}

/* How many function calls are nested; function_call() refuses to go
 * deeper than ACSH_MAX_CALL_DEPTH instead of exhausting the C stack. */
int env_get_call_depth(void) {
    return depth;
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
            /* quote markers are kept in the output: glob_expand() needs
             * them to leave quoted characters alone, and removes them */
            if (*p == SQ_START || *p == SQ_END) {
                in_single_quotes = (*p == SQ_START);
                if (len + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
                out[len++] = *p++;
                continue;
            }

            if (*p == ESCAPED_DOLLAR) {
                /* \$ inside "..." (see lexer.c's scan_word): always a
                 * literal '$', NEVER the start of a variable reference. */
                if (len + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
                out[len++] = '$';
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
                if (idx < env_positional_count()) {
                    {
                        const char *val = env_positional(idx + 1);
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
                int count = env_positional_count();
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
                {
                    int count = env_positional_count();
                    for (int i = 0; i < count; i++) {
                        if (i > 0) {
                            if (len + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
                            out[len++] = ' ';
                        }
                        const char *val = env_positional(i + 1);
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
                /* '$' not followed by a name or a parameter acsh knows
                 * (e.g. "$" alone, "$$", "$!"): emitted literally. */
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
