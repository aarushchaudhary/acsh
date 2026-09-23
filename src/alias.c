#define _POSIX_C_SOURCE 200809L  /* strdup under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "acsh.h"

static Alias alias_table[ACSH_MAX_ALIASES];

/* These are exactly the defaults the sourced community complaints
 * named as missing from ash (e.g. no `ll`), plus a couple of standard
 * conveniences most interactive shells ship. Loaded before ~/.acshrc
 * so the user can override any of them there. */
void alias_load_defaults(void) {
    alias_set("ll", "ls -l");
    alias_set("la", "ls -la");
    alias_set("..", "cd ..");
    alias_set("...", "cd ../..");
    alias_set("grep", "grep --color=auto");
}

void alias_set(const char *name, const char *value) {
    /* if it already exists, update in place */
    for (int i = 0; i < ACSH_MAX_ALIASES; i++) {
        if (alias_table[i].in_use && strcmp(alias_table[i].name, name) == 0) {
            strncpy(alias_table[i].value, value, sizeof(alias_table[i].value) - 1);
            alias_table[i].value[sizeof(alias_table[i].value) - 1] = '\0';
            return;
        }
    }
    /* otherwise find a free slot */
    for (int i = 0; i < ACSH_MAX_ALIASES; i++) {
        if (!alias_table[i].in_use) {
            alias_table[i].in_use = 1;
            strncpy(alias_table[i].name, name, sizeof(alias_table[i].name) - 1);
            alias_table[i].name[sizeof(alias_table[i].name) - 1] = '\0';
            strncpy(alias_table[i].value, value, sizeof(alias_table[i].value) - 1);
            alias_table[i].value[sizeof(alias_table[i].value) - 1] = '\0';
            return;
        }
    }
    fprintf(stderr, "acsh: alias table full\n");
}

void alias_unset(const char *name) {
    for (int i = 0; i < ACSH_MAX_ALIASES; i++) {
        if (alias_table[i].in_use && strcmp(alias_table[i].name, name) == 0) {
            alias_table[i].in_use = 0;
            return;
        }
    }
}

const char *alias_get(const char *name) {
    for (int i = 0; i < ACSH_MAX_ALIASES; i++) {
        if (alias_table[i].in_use && strcmp(alias_table[i].name, name) == 0) {
            return alias_table[i].value;
        }
    }
    return NULL;
}

void alias_print_all(void) {
    for (int i = 0; i < ACSH_MAX_ALIASES; i++) {
        if (alias_table[i].in_use) {
            printf("alias %s='%s'\n", alias_table[i].name, alias_table[i].value);
        }
    }
}

char *alias_expand_line(const char *line) {
    /* find the first word (up to the first space or end of string) */
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;

    const char *word_start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') p++;
    size_t word_len = (size_t)(p - word_start);

    if (word_len == 0 || word_len >= 64) {
        return NULL;
    }

    char first_word[64];
    memcpy(first_word, word_start, word_len);
    first_word[word_len] = '\0';

    const char *expansion = alias_get(first_word);
    if (expansion == NULL) {
        return NULL;
    }

    /* rebuild: <expansion><rest of original line starting at p> */
    size_t rest_len = strlen(p);
    size_t total_len = strlen(expansion) + rest_len + 1;
    char *result = malloc(total_len);
    if (result == NULL) {
        return NULL;
    }
    snprintf(result, total_len, "%s%s", expansion, p);
    return result;
}
