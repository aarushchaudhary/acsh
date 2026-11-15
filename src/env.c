#define _POSIX_C_SOURCE 200809L  /* strdup, strndup under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include "acsh.h"

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
typedef VarSave LocalVar;  /* the variable as it was before `local` */

typedef struct {
    char    **params;
    int       nparams;
    LocalVar *locals;
    int       nlocals;
} Frame;

static Frame *frames = NULL;  /* ACSH_MAX_CALL_DEPTH of them, allocated at the first call */
static int   depth = 0;

/* The script's own arguments ($1... outside any function): set from
 * the command line by main.c, changed by shift and set -- at the top. */
static Frame top_frame;

/* The frame $1, $#, shift and set -- act on right now. */
static Frame *current_frame(void) {
    return depth > 0 ? &frames[depth - 1] : &top_frame;
}

static char  *shell_name = NULL;      /* $0 */
static pid_t  shell_pid = 0;          /* $$: the same in every subshell */
static pid_t  last_background = 0;    /* $! */

static char **copy_params(int argc, char *argv[]) {
    char **params = malloc((size_t)(argc > 0 ? argc : 1) * sizeof(char *));
    if (params == NULL) {
        shell_error("out of memory\n");
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
    if (frames == NULL && (frames = calloc(ACSH_MAX_CALL_DEPTH, sizeof(Frame))) == NULL) {
        shell_error("out of memory\n");
        exit(2);
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
        f->locals[f->nlocals++] = var_save(name);
    }
    if (value != NULL) {
        return var_set(name, value) == 0 ? 0 : 2;
    }
    if (!known) {
        return var_unset(name) == 0 ? 0 : 2;  /* bare `local x` starts unset */
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
    for (int i = f->nlocals - 1; i >= 0; i--) {
        var_restore(&f->locals[i]);
    }
    free(f->locals);
    f->locals = NULL;
    f->nlocals = 0;
}

/* `set -- ARG...`: replaces the current parameters (the function's, or
 * the script's at the top level). */
int env_set_positional_params(int argc, char *argv[]) {
    Frame *f = current_frame();
    char **params = copy_params(argc, argv); /* before freeing: argv may be $@ itself */
    free_params(f);
    f->params = params;
    f->nparams = argc;
    return 0;
}

/* `shift [n]`. Returns 1 if n exceeds the parameters there are. */
int env_shift_positional_params(int n) {
    Frame *f = current_frame();
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
    return current_frame()->nparams;
}

const char *env_positional(int i) {
    if (i < 1 || i > env_positional_count()) {
        return "";
    }
    return current_frame()->params[i - 1];
}

/* $RANDOM: a new number 0..32767 on each read; assigning seeds it.
 * Seeded from the clock at the first read, not at startup: most runs
 * never read it, and reading the clock costs a page (the vDSO). */
static unsigned long random_state = 1;
static int random_seeded = 0;

void env_seed_random(unsigned long seed) {
    random_state = seed;
    random_seeded = 1;
}

static long next_random(void) {
    if (!random_seeded) {
        env_seed_random((unsigned long)time(NULL) ^ ((unsigned long)getpid() << 16));
    }
    random_state = random_state * 1103515245UL + 12345UL;
    return (long)((random_state >> 16) & 0x7fff);
}

/* $LINENO: the line of the command running now. */
static int current_line = 0;

void env_set_lineno(int line) {
    current_line = line;
}

int env_get_lineno(void) {
    return current_line;
}

void env_init(const char *name, int argc, char *argv[]) {
    free(shell_name);
    shell_name = strdup(name);
    shell_pid = getpid();
    env_set_positional_params(argc, argv);
}

void env_set_last_background(pid_t pid) {
    last_background = pid;
}

/* $1.., $*, $@ joined with single spaces (field splitting comes later). */
static char *join_params(void) {
    size_t total = 1;
    for (int i = 1; i <= env_positional_count(); i++) total += strlen(env_positional(i)) + 1;
    char *out = malloc(total);
    if (out == NULL) return NULL;
    out[0] = '\0';
    for (int i = 1; i <= env_positional_count(); i++) {
        if (i > 1) strcat(out, " ");
        strcat(out, env_positional(i));
    }
    return out;
}

static char *number_string(long n) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", n);
    return strdup(buf);
}

char *env_param_value(const char *name) {
    if (isdigit((unsigned char)name[0])) {
        for (const char *s = name; *s; s++) {
            if (!isdigit((unsigned char)*s)) return NULL; /* ${1x}: not a parameter */
        }
        long n = strtol(name, NULL, 10);
        if (n == 0) return strdup(shell_name != NULL ? shell_name : "acsh");
        if (n > env_positional_count()) return NULL;
        return strdup(env_positional((int)n));
    }
    if (name[1] == '\0') {
        switch (name[0]) {
        case '#': return number_string(env_positional_count());
        case '?': return number_string(last_exit_status);
        case '$': return number_string(shell_pid);
        case '!': return last_background > 0 ? number_string(last_background) : NULL;
        case '-': return strdup(shell_option_string());
        case '@': case '*': return join_params();
        }
    }
    if (strcmp(name, "RANDOM") == 0) return number_string(next_random());
    if (strcmp(name, "LINENO") == 0) return number_string(current_line);
    if (strcmp(name, "EPOCHSECONDS") == 0 || strcmp(name, "EPOCHREALTIME") == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        char buf[48];
        if (name[5] == 'S') snprintf(buf, sizeof(buf), "%lld", (long long)ts.tv_sec);
        else snprintf(buf, sizeof(buf), "%lld.%06ld", (long long)ts.tv_sec, ts.tv_nsec / 1000);
        return strdup(buf);
    }
    const char *value = var_get(name);
    return value != NULL ? strdup(value) : NULL;
}

/* How many function calls are nested; function_call() refuses to go
 * deeper than ACSH_MAX_CALL_DEPTH instead of exhausting the C stack. */
int env_get_call_depth(void) {
    return depth;
}
