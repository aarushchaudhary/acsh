#define _POSIX_C_SOURCE 200809L  /* strdup under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glob.h>
#include "acsh.h"

/* Must match parser.c's sentinel bytes. See parser.c's tokenizer
 * comment and env.c's matching definitions for the full explanation
 * of why single-quoted regions are marked this way. */
#define SQ_START     '\x01'
#define SQ_END       '\x02'
#define QUOTED_FIRST '\x03'

/* Returns 1 if `word` contains at least one glob metacharacter that
 * is NOT inside a single-quoted region (SQ_START..SQ_END). This is a
 * deliberate simplification: it correctly handles the common cases
 * (`*.c` expands, `'*.c'` does not), but a glob character inside
 * DOUBLE quotes (e.g. echo "*.c") is NOT currently distinguished from
 * a bare one, because parser.c only sentinel-marks single-quoted
 * text, not double-quoted text, in the general (non-first-character)
 * position. Documented as a known scope limitation. */
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

int glob_expand(const char *word, char *results[], int max_results) {
    if (max_results <= 0) {
        return 0;
    }

    if (!has_unquoted_glob_chars(word)) {
        /* nothing to expand -- just strip sentinels and return the
         * word as-is, as the single result */
        results[0] = strip_sentinels(word);
        return (results[0] != NULL) ? 1 : 0;
    }

    char *pattern = strip_sentinels(word);
    if (pattern == NULL) {
        return 0;
    }

    glob_t g;
    int rc = glob(pattern, 0, NULL, &g);

    if (rc == GLOB_NOMATCH) {
        /* POSIX rule: no match -> the word stays as its own literal
         * text, unexpanded and unremoved */
        results[0] = pattern;
        return 1;
    }

    if (rc != 0) {
        /* GLOB_NOSPACE or GLOB_ABORTED -- treat like no-match rather
         * than failing the whole command over a glob hiccup */
        results[0] = pattern;
        return 1;
    }

    int n = 0;
    for (size_t i = 0; i < g.gl_pathc && n < max_results; i++) {
        results[n] = strdup(g.gl_pathv[i]);
        n++;
    }

    globfree(&g);
    free(pattern);
    return n;
}
