#define _POSIX_C_SOURCE 200809L  /* strdup under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "acsh.h"

/* Aliases, in the order they were defined (as `alias` lists them).
 * The table grows as needed and values may be any length. */
typedef struct {
    char *name;
    char *value;
} Alias;

static Alias *aliases = NULL;
static int    naliases = 0, cap = 0;

static int find(const char *name) {
    for (int i = 0; i < naliases; i++) {
        if (strcmp(aliases[i].name, name) == 0) return i;
    }
    return -1;
}

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
    char *copy = strdup(value);
    if (copy == NULL) {
        shell_error("out of memory\n");
        return;
    }
    int i = find(name);
    if (i >= 0) {
        free(aliases[i].value);
        aliases[i].value = copy;
        return;
    }
    if (naliases == cap) {
        int ncap = cap ? cap * 2 : 16;
        Alias *bigger = realloc(aliases, (size_t)ncap * sizeof(Alias));
        if (bigger == NULL) {
            free(copy);
            shell_error("out of memory\n");
            return;
        }
        aliases = bigger;
        cap = ncap;
    }
    aliases[naliases].name = strdup(name);
    aliases[naliases].value = copy;
    if (aliases[naliases].name == NULL) {
        free(copy);
        shell_error("out of memory\n");
        return;
    }
    naliases++;
}

void alias_unset(const char *name) {
    int i = find(name);
    if (i < 0) return;
    free(aliases[i].name);
    free(aliases[i].value);
    memmove(aliases + i, aliases + i + 1, (size_t)(naliases - i - 1) * sizeof(Alias));
    naliases--;
}

const char *alias_get(const char *name) {
    int i = find(name);
    return i >= 0 ? aliases[i].value : NULL;
}

/* name='value' in ash's form */
void alias_print(const char *name, const char *value) {
    fputs(name, stdout);
    putchar('=');
    print_single_quoted(value);
    putchar('\n');
}

void alias_print_all(void) {
    for (int i = 0; i < naliases; i++) alias_print(aliases[i].name, aliases[i].value);
}

/* Every alias name, for Tab completion. */
void alias_each(void (*fn)(const char *name, void *ctx), void *ctx) {
    for (int i = 0; i < naliases; i++) fn(aliases[i].name, ctx);
}
