#define _DEFAULT_SOURCE           /* d_type */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "acsh.h"

/* Tab completion. What the word at the cursor completes to depends on
 * where it is:
 *
 *   $NAME, ${NAME            variable names
 *   the command position     commands in $PATH, builtins, aliases,
 *                            functions and reserved words (a word with
 *                            a / in it: a path)
 *   after cd                 directories only
 *   the word after apk       apk's subcommands
 *   anything else            files and directories (~/ understood)
 *
 * One match is inserted whole, followed by a space (a / for a
 * directory). Several matches are extended to what they have in common;
 * if that adds nothing, they are listed below the line. */

typedef struct {
    char **v;
    int n, cap;
    const char *prefix;
    size_t plen;
} Cands;

static void add(Cands *c, const char *name) {
    if (strncmp(name, c->prefix, c->plen) != 0) return;
    for (int i = 0; i < c->n; i++) {
        if (strcmp(c->v[i], name) == 0) return;
    }
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->v = realloc(c->v, (size_t)c->cap * sizeof(char *));
    }
    c->v[c->n++] = strdup(name);
}

static void add_cb(const char *name, void *ctx) {
    add((Cands *)ctx, name);
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Characters a completed word must escape to stay one word. */
static int needs_escape(char c) {
    return strchr(" \t'\"$*?\\;&()|<>!{}[]^`#~", c) != NULL;
}

static void add_commands(Cands *c) {
    static const char *reserved[] = { "if", "then", "else", "elif", "fi", "do", "done", "case",
                                      "esac", "while", "until", "for", "in", "function", NULL };
    for (int i = 0; reserved[i]; i++) add(c, reserved[i]);
    builtin_each(add_cb, c);
    alias_each(add_cb, c);
    function_each(add_cb, c);
    const char *path = var_get("PATH");
    if (path == NULL) return;
    char *copy = strdup(path);
    for (char *dir = copy, *next; dir != NULL; dir = next) {
        next = strchr(dir, ':');
        if (next != NULL) *next++ = '\0';
        DIR *d = opendir(*dir ? dir : ".");
        if (d == NULL) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.' || strncmp(e->d_name, c->prefix, c->plen) != 0) continue;
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", *dir ? dir : ".", e->d_name);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0) add(c, e->d_name);
        }
        closedir(d);
    }
    free(copy);
}

/* Files under the directory part of word (as typed, ~/ allowed); the
 * candidates are whole words: directory part + name (+ / for a dir). */
static void add_files(Cands *c, const char *word, int dirs_only, int exec_only) {
    const char *slash = strrchr(word, '/');
    size_t dlen = slash ? (size_t)(slash - word) + 1 : 0;
    char dir[4096];
    if (dlen == 0) {
        strcpy(dir, ".");
    } else if (word[0] == '~' && (word[1] == '/' || dlen == 2)) {
        const char *home = var_get("HOME");
        snprintf(dir, sizeof(dir), "%s%.*s", home ? home : "", (int)(dlen - 1), word + 1);
    } else {
        snprintf(dir, sizeof(dir), "%.*s", (int)dlen, word);
    }
    const char *base = word + dlen;
    size_t blen = strlen(base);
    DIR *d = opendir(dir);
    if (d == NULL) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, base, blen) != 0) continue;
        if (e->d_name[0] == '.' && base[0] != '.') continue;   /* hidden unless asked */
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[8300];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        int is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        if (dirs_only && !is_dir) continue;
        if (exec_only && !is_dir && access(full, X_OK) != 0) continue;
        char cand[8300];
        snprintf(cand, sizeof(cand), "%.*s%s%s", (int)dlen, word, e->d_name, is_dir ? "/" : "");
        add(c, cand);
    }
    closedir(d);
}

static void add_vars(Cands *c) {
    var_each(add_cb, c);
    static const char *special[] = { "RANDOM", "LINENO", "EPOCHSECONDS", "EPOCHREALTIME", NULL };
    for (int i = 0; special[i]; i++) add(c, special[i]);
}

static void add_apk(Cands *c) {
    static const char *sub[] = { "add", "audit", "cache", "del", "dot", "fetch", "fix", "index",
                                 "info", "list", "manifest", "policy", "search", "stats",
                                 "update", "upgrade", "verify", "version", NULL };
    for (int i = 0; sub[i]; i++) add(c, sub[i]);
}

/* The candidates in columns, under the line being typed. */
static void list_candidates(Cands *c, size_t strip) {
    size_t width = 0;
    for (int i = 0; i < c->n; i++) {
        size_t l = strlen(c->v[i] + strip);
        if (l > width) width = l;
    }
    width += 2;
    size_t per_row = 80 / width ? 80 / width : 1;
    fputs("\r\n", stderr);
    for (int i = 0; i < c->n; i++) {
        fprintf(stderr, "%-*s", (int)width, c->v[i] + strip);
        if ((size_t)(i + 1) % per_row == 0 || i == c->n - 1) fputs("\r\n", stderr);
    }
    fflush(stderr);
}

char *lineedit_complete(const char *line, size_t pos, size_t *new_pos) {
    /* the word at the cursor: back to a blank or an operator, skipping \x */
    size_t start = pos;
    while (start > 0) {
        char p = line[start - 1];
        if (start >= 2 && line[start - 2] == '\\') { start -= 2; continue; }
        if (strchr(" \t;|&(<>`", p)) break;
        start--;
    }
    /* the word as the shell will see it: backslashes removed */
    char *word = malloc(pos - start + 1);
    size_t wl = 0;
    for (size_t i = start; i < pos; i++) {
        if (line[i] == '\\' && i + 1 < pos) i++;
        word[wl++] = line[i];
    }
    word[wl] = '\0';

    /* what comes before it: the command position, or which command */
    size_t b = start;
    while (b > 0 && (line[b - 1] == ' ' || line[b - 1] == '\t')) b--;
    int command_pos = (b == 0 || strchr(";|&(`", line[b - 1]) != NULL);
    char first[64] = "";
    if (!command_pos) {
        size_t f = b;
        while (f > 0 && !strchr(";|&(`", line[f - 1])) f--;
        while (line[f] == ' ' || line[f] == '\t') f++;
        size_t fl = 0;
        while (f + fl < b && line[f + fl] != ' ' && line[f + fl] != '\t' && fl + 1 < sizeof(first)) fl++;
        memcpy(first, line + f, fl);
        first[fl] = '\0';
    }
    int second_word = 0;   /* the word right after the command name */
    if (!command_pos) {
        size_t f = b;
        while (f > 0 && line[f - 1] != ' ' && line[f - 1] != '\t' && !strchr(";|&(`", line[f - 1])) f--;
        size_t g = f;
        while (g > 0 && (line[g - 1] == ' ' || line[g - 1] == '\t')) g--;
        second_word = (g == 0 || strchr(";|&(`", line[g - 1]) != NULL);
    }

    Cands c = { NULL, 0, 0, word, wl };
    size_t keep = 0;        /* leading characters of a candidate that are not in the word ($, ${) */
    const char *var_start = strrchr(word, '$');
    if (var_start != NULL && (var_start[1] == '{' ? !strchr(var_start, '}') : 1) &&
        strspn(var_start + 1 + (var_start[1] == '{'), "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") ==
            strlen(var_start + 1 + (var_start[1] == '{'))) {
        /* $NAME or ${NAME: complete the name, keeping what came before */
        keep = (size_t)(var_start - word) + 1 + (var_start[1] == '{');
        c.prefix = word + keep;
        c.plen = wl - keep;
        add_vars(&c);
    } else if (command_pos && strchr(word, '/') == NULL) {
        add_commands(&c);
    } else if (command_pos) {
        add_files(&c, word, 0, 1);
    } else if (strcmp(first, "cd") == 0 || strcmp(first, "pushd") == 0) {
        add_files(&c, word, 1, 0);
    } else if (strcmp(first, "apk") == 0 && second_word && word[0] != '-') {
        add_apk(&c);
    } else {
        add_files(&c, word, 0, 0);
    }
    char *result = NULL;
    if (c.n > 0) {
        qsort(c.v, (size_t)c.n, sizeof(char *), by_name);
        /* what all candidates share */
        size_t common = strlen(c.v[0]);
        for (int i = 1; i < c.n; i++) {
            size_t k = 0;
            while (k < common && c.v[i][k] == c.v[0][k]) k++;
            common = k;
        }
        size_t typed_len = wl - keep;
        int unique = (c.n == 1);
        if (common > typed_len || unique) {
            /* the new word: what came before the name, then the name, escaped */
            const char *name = c.v[0];
            size_t nl = unique ? strlen(name) : common;
            char *ins = malloc((nl + 1) * 2 + keep + 4);
            size_t k = 0;
            for (size_t i = 0; i < keep; i++) ins[k++] = word[i];
            for (size_t i = 0; i < nl; i++) {
                if (needs_escape(name[i]) && !(i == 0 && name[i] == '~' && keep == 0 && word[0] == '~')) ins[k++] = '\\';
                ins[k++] = name[i];
            }
            if (unique) {
                if (keep > 0 && word[keep - 1] == '{') ins[k++] = '}';
                else if (nl == 0 || name[nl - 1] != '/') ins[k++] = ' ';
            }
            ins[k] = '\0';
            size_t tail = strlen(line + pos);
            result = malloc(start + k + tail + 1);
            memcpy(result, line, start);
            memcpy(result + start, ins, k);
            memcpy(result + start + k, line + pos, tail + 1);
            *new_pos = start + k;
            free(ins);
        } else {
            /* nothing to add: show the choices (only the last part of a path) */
            const char *sl = strrchr(word, '/');
            list_candidates(&c, (keep == 0 && sl != NULL) ? (size_t)(sl - word) + 1 : 0);
        }
    }
    for (int i = 0; i < c.n; i++) free(c.v[i]);
    free(c.v);
    free(word);
    return result;
}
