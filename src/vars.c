#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "acsh.h"

/* Shell variables. Every variable lives here, with flags: exported ones
 * go into the environment of commands acsh runs (built at exec time by
 * var_environ()), the rest stay inside the shell. The process's own
 * environment is only read once, at startup. */

extern char **environ;

typedef struct Var {
    char       *name;
    char       *value;  /* NULL: declared (export x / readonly x) but unset */
    int         flags;  /* V_EXPORT, V_READONLY */
    struct Var *next;
} Var;

#define NBUCKETS 256
static Var *table[NBUCKETS];

static unsigned hash(const char *s) {
    unsigned h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h % NBUCKETS;
}

static Var *find(const char *name) {
    for (Var *v = table[hash(name)]; v != NULL; v = v->next) {
        if (strcmp(v->name, name) == 0) return v;
    }
    return NULL;
}

static Var *create(const char *name) {
    Var *v = calloc(1, sizeof(Var));
    if (v == NULL || (v->name = strdup(name)) == NULL) {
        shell_error("out of memory\n");
        exit(2);
    }
    unsigned h = hash(name);
    v->next = table[h];
    table[h] = v;
    return v;
}

int var_valid_name(const char *name) {
    if (!(isalpha((unsigned char)*name) || *name == '_')) return 0;
    for (const char *s = name + 1; *s; s++) {
        if (!(isalnum((unsigned char)*s) || *s == '_')) return 0;
    }
    return 1;
}

void vars_init(void) {
    for (char **e = environ; e != NULL && *e != NULL; e++) {
        const char *eq = strchr(*e, '=');
        if (eq == NULL) continue;
        char *name = strndup(*e, (size_t)(eq - *e));
        if (name != NULL && var_valid_name(name) && find(name) == NULL) {
            Var *v = create(name);
            v->value = strdup(eq + 1);
            v->flags = V_EXPORT;
        }
        free(name);
    }
}

const char *var_get(const char *name) {
    Var *v = find(name);
    return v != NULL ? v->value : NULL;
}

int var_flags(const char *name) {
    Var *v = find(name);
    return v != NULL ? v->flags : 0;
}

int var_set(const char *name, const char *value) {
    Var *v = find(name);
    if (v != NULL && (v->flags & V_READONLY)) {
        shell_error("%s: is read only\n", name);
        return -1;
    }
    if (v == NULL) v = create(name);
    char *copy = strdup(value != NULL ? value : "");
    if (copy == NULL) {
        shell_error("out of memory\n");
        exit(2);
    }
    free(v->value);
    v->value = copy;
    if (shell_option('a')) v->flags |= V_EXPORT; /* set -a */
    if (strcmp(name, "RANDOM") == 0) env_seed_random(strtoul(copy, NULL, 10));
    return 0;
}

int var_unset(const char *name) {
    unsigned h = hash(name);
    for (Var **pp = &table[h]; *pp != NULL; pp = &(*pp)->next) {
        Var *v = *pp;
        if (strcmp(v->name, name) != 0) continue;
        if (v->flags & V_READONLY) {
            shell_error("unset: %s: is read only\n", name);
            return -1;
        }
        *pp = v->next;
        free(v->name);
        free(v->value);
        free(v);
        return 0;
    }
    return 0;
}

void var_add_flags(const char *name, int flags) {
    Var *v = find(name);
    if (v == NULL) v = create(name);
    v->flags |= flags;
}

void var_clear_flags(const char *name, int flags) {
    Var *v = find(name);
    if (v != NULL) v->flags &= ~flags;
}

VarSave var_save(const char *name) {
    VarSave s = { strdup(name), NULL, 0, 0 };
    Var *v = find(name);
    if (v != NULL) {
        s.existed = 1;
        s.flags = v->flags;
        s.value = v->value != NULL ? strdup(v->value) : NULL;
    }
    return s;
}

void var_restore(VarSave *s) {
    Var *v = find(s->name);
    if (!s->existed) {
        if (v != NULL) {
            v->flags &= ~V_READONLY;
            var_unset(s->name);
        }
    } else {
        if (v == NULL) v = create(s->name);
        free(v->value);
        v->value = s->value;
        v->flags = s->flags;
        s->value = NULL;
    }
    free(s->name);
    free(s->value);
    s->name = s->value = NULL;
}

char **var_environ(void) {
    size_t n = 0;
    for (int b = 0; b < NBUCKETS; b++) {
        for (Var *v = table[b]; v != NULL; v = v->next) {
            if ((v->flags & V_EXPORT) && v->value != NULL) n++;
        }
    }
    char **env = malloc((n + 1) * sizeof(char *));
    if (env == NULL) return NULL;
    size_t k = 0;
    for (int b = 0; b < NBUCKETS; b++) {
        for (Var *v = table[b]; v != NULL; v = v->next) {
            if (!(v->flags & V_EXPORT) || v->value == NULL) continue;
            size_t len = strlen(v->name) + strlen(v->value) + 2;
            env[k] = malloc(len);
            if (env[k] == NULL) continue;
            snprintf(env[k], len, "%s=%s", v->name, v->value);
            k++;
        }
    }
    env[k] = NULL;
    return env;
}

/* Writes value in single quotes, as ash does ('it'"'"'s' for it's). */
static void print_quoted(const char *value) {
    putchar('\'');
    for (const char *s = value; *s; s++) {
        if (*s == '\'') fputs("'\"'\"'", stdout);
        else putchar(*s);
    }
    putchar('\'');
}

static int by_name(const void *a, const void *b) {
    return strcmp((*(Var *const *)a)->name, (*(Var *const *)b)->name);
}

void var_print(int which) {
    size_t n = 0;
    for (int b = 0; b < NBUCKETS; b++)
        for (Var *v = table[b]; v != NULL; v = v->next) n++;
    Var **all = malloc((n + 1) * sizeof(Var *));
    if (all == NULL) return;
    size_t k = 0;
    for (int b = 0; b < NBUCKETS; b++)
        for (Var *v = table[b]; v != NULL; v = v->next) all[k++] = v;
    qsort(all, k, sizeof(Var *), by_name);
    for (size_t i = 0; i < k; i++) {
        Var *v = all[i];
        if (which == VARS_EXPORTED && !(v->flags & V_EXPORT)) continue;
        if (which == VARS_READONLY && !(v->flags & V_READONLY)) continue;
        if (which == VARS_ALL && v->value == NULL) continue;
        if (which == VARS_EXPORTED) fputs("export ", stdout);
        if (which == VARS_READONLY) fputs("readonly ", stdout);
        fputs(v->name, stdout);
        if (v->value != NULL) {
            putchar('=');
            print_quoted(v->value);
        }
        putchar('\n');
    }
    free(all);
}
