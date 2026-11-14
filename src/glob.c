#define _POSIX_C_SOURCE 200809L  /* strdup under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glob.h>
#include "acsh.h"

#include "syntax.h"  /* the marker bytes inside words */

/* Returns 1 if `word` has a glob metacharacter outside the quoted
 * regions (SQ_START..SQ_END) the lexer marks: `*.c` expands, while
 * '*.c', "*.c" and \*.c do not. One gap remains: characters that come
 * from a variable expanded inside double quotes ("$x" with x='*') are
 * not marked, so they can still glob. */
static int has_unquoted_glob_chars(const char *word) {
    int in_single_quotes = 0;
    for (const char *p = word; *p != '\0'; p++) {
        if (*p == SQ_START) { in_single_quotes = 1; continue; }
        if (*p == SQ_END)   { in_single_quotes = 0; continue; }
        if (in_single_quotes) continue;
        if (*p == '*' || *p == '?' || *p == '[') {
            return 1;
        }
    }
    return 0;
}

/* Strips the QUOTED_FIRST marker and all SQ_START/SQ_END sentinels
 * out of `word`, producing the plain string glob()/argv should
 * actually see. Returns a newly malloc'd string. */
static char *strip_sentinels(const char *word) {
    char *out = malloc(strlen(word) + 1);
    size_t len = 0;
    if (out == NULL) {
        return NULL;
    }
    for (const char *p = word; *p != '\0'; p++) {
        if (*p == SQ_START || *p == SQ_END || *p == QUOTED_FIRST) {
            continue;
        }
        out[len++] = *p;
    }
    out[len] = '\0';
    return out;
}

int glob_expand(const char *word, char ***results) {
    *results = NULL;
    char *pattern = strip_sentinels(word);
    if (pattern == NULL) {
        return 0;
    }

    glob_t g;
    if (!has_unquoted_glob_chars(word) || glob(pattern, 0, NULL, &g) != 0) {
        /* no glob characters, no match, or a glob error: POSIX keeps the
         * word as it is (no match is not an error) */
        char **one = malloc(sizeof(char *));
        if (one == NULL) {
            free(pattern);
            return 0;
        }
        one[0] = pattern;
        *results = one;
        return 1;
    }
    free(pattern);

    char **out = malloc(g.gl_pathc * sizeof(char *));
    if (out == NULL) {
        globfree(&g);
        return 0;
    }
    for (size_t i = 0; i < g.gl_pathc; i++) {
        out[i] = strdup(g.gl_pathv[i]);
    }
    int n = (int)g.gl_pathc;
    globfree(&g);
    *results = out;
    return n;
}
