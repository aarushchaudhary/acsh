#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "acsh.h"

typedef struct {
    char  name[64];
    char *body;       /* heap-allocated raw body text, between { and } */
    int   in_use;
} ShellFunction;

static ShellFunction function_table[ACSH_MAX_FUNCTIONS];

/* Returns 1 if c can appear in a shell function/variable name
 * ([A-Za-z0-9_]). Function names follow the same rules as variable
 * names in POSIX shells. */
static int is_name_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

int is_function_definition(const char *line) {
    while (*line == ' ' || *line == '\t') line++;

    size_t i = 0;
    while (is_name_char(line[i])) i++;
    if (i == 0) {
        return 0; /* doesn't even start with a valid name character */
    }

    /* after the name, optional whitespace, then "()" */
    size_t j = i;
    while (line[j] == ' ' || line[j] == '\t') j++;
    if (line[j] != '(') return 0;
    j++;
    while (line[j] == ' ' || line[j] == '\t') j++;
    if (line[j] != ')') return 0;

    return 1;
}

/* Finds the matching closing '}' for an opening '{' at *p, using the
 * same quote-aware, nesting-aware scanning technique used throughout
 * control_flow.c (nested braces from e.g. a nested function
 * definition, or just literal '{'/'}' in a pattern, are depth-
 * tracked). Returns NULL if not found before `limit`. */
static const char *find_matching_brace(const char *open_brace, const char *limit) {
    const char *p = open_brace + 1;
    char in_quote = '\0';
    int depth = 1;

    while (p < limit) {
        if (in_quote != '\0') {
            if (*p == in_quote) in_quote = '\0';
            p++;
            continue;
        }
        if (*p == '\'' || *p == '"') { in_quote = *p; p++; continue; }
        if (*p == '{') { depth++; p++; continue; }
        if (*p == '}') {
            depth--;
            if (depth == 0) {
                return p;
            }
            p++;
            continue;
        }
        p++;
    }
    return NULL;
}

static ShellFunction *find_function(const char *name) {
    for (int i = 0; i < ACSH_MAX_FUNCTIONS; i++) {
        if (function_table[i].in_use && strcmp(function_table[i].name, name) == 0) {
            return &function_table[i];
        }
    }
    return NULL;
}

int function_is_defined(const char *name) {
    return find_function(name) != NULL;
}

static void store_function(const char *name, const char *body_start, size_t body_len) {
    ShellFunction *slot = find_function(name);
    if (slot == NULL) {
        for (int i = 0; i < ACSH_MAX_FUNCTIONS; i++) {
            if (!function_table[i].in_use) {
                slot = &function_table[i];
                break;
            }
        }
    }
    if (slot == NULL) {
        fprintf(stderr, "acsh: function table full, cannot define '%s'\n", name);
        return;
    }

    free(slot->body); /* safe no-op if redefining and body was NULL before */
    slot->body = malloc(body_len + 1);
    if (slot->body != NULL) {
        memcpy(slot->body, body_start, body_len);
        slot->body[body_len] = '\0';
    }
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';
    slot->in_use = 1;
}

int run_function_definition(const char *first_line,
                            char *(*read_more_line)(char *buf, int size)) {
    const char *p = first_line;
    while (*p == ' ' || *p == '\t') p++;

    size_t name_len = 0;
    while (is_name_char(p[name_len])) name_len++;
    if (name_len == 0 || name_len >= 64) {
        fprintf(stderr, "acsh: syntax error: invalid function name\n");
        return 1;
    }
    char name[64];
    memcpy(name, p, name_len);
    name[name_len] = '\0';

    /* Accumulate lines (like control_flow.c's constructs do) until we
     * have both the opening '{' and its matching closing '}'. The
     * opening brace commonly appears on the SAME line as "name()" in
     * compact style, or on the FOLLOWING line in traditional
     * script style -- both are handled by just reading more lines
     * until a '{' shows up, then more until its matching '}' shows up. */
    size_t cap = ACSH_MAX_LINE * 4;
    char *buf = malloc(cap);
    if (buf == NULL) {
        fprintf(stderr, "acsh: out of memory\n");
        return 1;
    }
    size_t len = 0;
    {
        size_t plen = strlen(first_line);
        while (len + plen + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        memcpy(buf + len, first_line, plen);
        len += plen;
        buf[len++] = '\n';
    }

    const char *open_brace = NULL;
    for (const char *q = buf; q < buf + len; q++) {
        if (*q == '{') { open_brace = q; break; }
    }
    while (open_brace == NULL) {
        char linebuf[ACSH_MAX_LINE];
        if (read_more_line(linebuf, sizeof(linebuf)) == NULL) {
            fprintf(stderr, "acsh: syntax error: expected '{' in function definition\n");
            free(buf);
            return 1;
        }
        size_t llen = strlen(linebuf);
        while (len + llen + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        memcpy(buf + len, linebuf, llen);
        len += llen;
        if (len == 0 || buf[len - 1] != '\n') buf[len++] = '\n';

        for (const char *q = buf; q < buf + len; q++) {
            if (*q == '{') { open_brace = q; break; }
        }
    }

    const char *close_brace = find_matching_brace(open_brace, buf + len);
    while (close_brace == NULL) {
        char linebuf[ACSH_MAX_LINE];
        if (read_more_line(linebuf, sizeof(linebuf)) == NULL) {
            fprintf(stderr, "acsh: syntax error: expected '}' to close function '%s'\n", name);
            free(buf);
            return 1;
        }
        size_t llen = strlen(linebuf);
        while (len + llen + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        memcpy(buf + len, linebuf, llen);
        len += llen;
        if (len == 0 || buf[len - 1] != '\n') buf[len++] = '\n';

        close_brace = find_matching_brace(open_brace, buf + len);
    }

    const char *body_start = open_brace + 1;
    size_t body_len = (size_t)(close_brace - body_start);
    store_function(name, body_start, body_len);

    free(buf);
    return 0;
                            }

                            int function_call(int argc, char *argv[]) {
                                ShellFunction *fn = find_function(argv[0]);
                                if (fn == NULL) {
                                    fprintf(stderr, "acsh: %s: function not found\n", argv[0]);
                                    return 127;
                                }

                                /* Refuse to recurse past the depth limit. Without this check, a
                                 * runaway recursive function (e.g. one whose base case is never
                                 * reached) keeps nesting real C stack frames --
                                 * function_call() -> run_statements() -> execute_chain() ->
                                 * function_call() ... -- until the process's actual stack is
                                 * exhausted and it segfaults. Checking here turns that crash into
                                 * an ordinary, recoverable shell error, the same way real shells
                                 * cap function nesting depth. */
                                if (env_get_call_depth() >= ACSH_MAX_CALL_DEPTH) {
                                    fprintf(stderr, "acsh: %s: maximum function nesting depth (%d) exceeded\n",
                                            argv[0], ACSH_MAX_CALL_DEPTH);
                                    return 1;
                                }

                                /* $1, $2, ... are the CALL'S ARGUMENTS, not including the function
                                 * name itself (argv[0]) -- e.g. `greet Alice 30` must set $1 to
                                 * "Alice", not to "greet". Pass argv+1 / argc-1 to the positional
                                 * parameter stack accordingly. */
                                env_push_positional_params(argc - 1, argv + 1);
                                int status = run_statements_entrypoint(fn->body, fn->body + strlen(fn->body));
                                env_pop_positional_params();

                                return status;
                            }
