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

            if (*p != '$' || in_single_quotes) {
                if (len + 1 >= cap) {
                    cap *= 2;
                    out = realloc(out, cap);
                }
                out[len++] = *p++;
                continue;
            }

            /* *p == '$' and we are NOT inside single quotes */
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
