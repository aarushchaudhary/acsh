#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include "acsh.h"
#include "syntax.h"

/* Runs the syntax tree parse.c builds. */

/* ---- break, continue, return ---- */

enum { LOOP_NONE, LOOP_BREAK, LOOP_CONTINUE };
static int loop_signal = LOOP_NONE;
static int loop_level = 0;     /* how many loops the break/continue still has to leave */
static int loop_depth = 0;     /* loops currently running */
static int return_pending = 0;
static int return_value = 0;

void control_flow_signal_loop(int is_continue, int level) {
    loop_signal = is_continue ? LOOP_CONTINUE : LOOP_BREAK;
    loop_level = (level >= 1) ? level : 1;
    if (loop_level > loop_depth) {
        loop_level = loop_depth; /* `break 5` in 2 loops leaves both, as in ash */
    }
}

int control_flow_in_loop_depth(void) {
    return loop_depth;
}

void control_flow_signal_return(int status) {
    return_pending = 1;
    return_value = status;
}

int control_flow_take_return(int *status) {
    if (!return_pending) {
        return 0;
    }
    return_pending = 0;
    *status = return_value;
    return 1;
}

int control_flow_unwinding(void) {
    return loop_signal != LOOP_NONE || return_pending || shell_interrupted();
}

void control_flow_reset(void) {
    loop_signal = LOOP_NONE;
    loop_level = 0;
    return_pending = 0;
}

/* After a loop's condition or body ran: what should this loop do? */
enum { STAY, LEAVE, NEXT };
static int loop_check(void) {
    if (return_pending || shell_interrupted()) {
        return LEAVE;
    }
    if (loop_signal == LOOP_NONE) {
        return STAY;
    }
    if (loop_level > 1) {
        loop_level--;      /* meant for an outer loop: leave this one, keep it pending */
        return LEAVE;
    }
    int sig = loop_signal;
    loop_signal = LOOP_NONE;
    loop_level = 0;
    return sig == LOOP_BREAK ? LEAVE : NEXT;
}

/* ---- processes ---- */

/* 0 inside forked children: no process groups, terminal or job table
 * handling there; the parent shell owns the job. */
static int job_control = 1;

int status_to_exit_code(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 0;
}

/* Common setup in every forked child: default signals, SIGCHLD
 * unblocked, no job control, not interactive, no inherited traps. */
void child_setup_subshell(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    unblock_sigchld();
    job_control = 0;
    shell_mark_subshell();      /* not interactive: no prompts or job notices */
    trap_set_int(NULL);         /* traps are not inherited by subshells */
    trap_set_exit(NULL);
}

static void child_exit(int status) {
    /* only the output streams: flushing an input stream would move the
     * file offset the parent shell is still reading its script from */
    fflush(stdout);
    fflush(stderr);
    _exit(status & 0xff);
}

/* ---- expansion ---- */

/* A word expanded without field splitting or globbing (assignment
 * values, redirection targets, case words), quotes removed. */
static char *expand_plain(const char *word) {
    char *e = env_expand(word);
    if (e == NULL) {
        return strdup("");
    }
    char *out = e;
    for (char *s = e; *s; s++) {
        if (*s != SQ_START && *s != SQ_END && *s != QUOTED_FIRST) {
            *out++ = *s;
        }
    }
    *out = '\0';
    return e;
}

typedef struct {
    char **v;
    int    n, cap;
} Args;

static void args_push(Args *a, char *s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->v = realloc(a->v, (size_t)a->cap * sizeof(char *));
        if (a->v == NULL) {
            fprintf(stderr, "acsh: out of memory\n");
            exit(2);
        }
    }
    a->v[a->n++] = s;
    a->v[a->n] = NULL;
}

static void args_free(Args *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    a->v = NULL;
    a->n = a->cap = 0;
}

static int has_quotes(const char *s) {
    for (; *s; s++) {
        if (*s == SQ_START || *s == QUOTED_FIRST) return 1;
    }
    return 0;
}

/* Globs one expanded field and appends the results. */
static void push_globbed(Args *a, const char *field) {
    char **matches;
    int n = glob_expand(field, &matches);
    for (int i = 0; i < n; i++) {
        args_push(a, matches[i]);
    }
    free(matches);
}

/* Expands words into arguments. An unquoted word that expands to
 * nothing yields no argument, as in POSIX. With split, the expansion is
 * also split on unquoted blanks (used for `for` word lists). */
static void expand_words(char **words, int nwords, int split, Args *out) {
    for (int i = 0; i < nwords; i++) {
        char *e = env_expand(words[i]);
        if (e == NULL) continue;
        if (*e == '\0' && !has_quotes(words[i])) {
            free(e);
            continue;
        }
        if (!split) {
            push_globbed(out, e);
            free(e);
            continue;
        }
        int quoted = 0;
        int fields = 0;
        char *field = e;
        for (char *s = e;; s++) {
            if (*s == SQ_START) quoted = 1;
            else if (*s == SQ_END) quoted = 0;
            int blank = !quoted && (*s == ' ' || *s == '\t' || *s == '\n');
            if (*s == '\0' || blank) {
                char saved = *s;
                *s = '\0';
                if (s > field) {
                    push_globbed(out, field);
                    fields++;
                }
                if (saved == '\0') break;
                field = s + 1;
            }
        }
        if (fields == 0 && has_quotes(words[i])) {
            args_push(out, strdup("")); /* "" is one empty field */
        }
        free(e);
    }
}

/* A case pattern for fnmatch(): quoted characters become literal. */
static char *case_pattern(const char *word) {
    char *e = env_expand(word);
    if (e == NULL) return strdup("");
    char *out = malloc(strlen(e) * 2 + 1);
    char *o = out;
    int quoted = 0;
    for (char *s = e; *s; s++) {
        if (*s == SQ_START) { quoted = 1; continue; }
        if (*s == SQ_END) { quoted = 0; continue; }
        if (*s == QUOTED_FIRST) continue;
        if (quoted && strchr("*?[]\\", *s)) *o++ = '\\';
        *o++ = *s;
    }
    *o = '\0';
    free(e);
    return out;
}

static void set_assignment(const char *assign) {
    const char *eq = strchr(assign, '=');
    char *name = strndup(assign, (size_t)(eq - assign));
    char *value = expand_plain(eq + 1);
    setenv(name, value, 1);
    free(name);
    free(value);
}

/* ---- redirections ---- */

typedef struct {
    int saved[10];  /* copy of the original fd, -1 untouched, -2 was closed */
} FdSave;

static void save_fd(FdSave *s, int fd) {
    if (s == NULL || s->saved[fd] != -1) return;
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    s->saved[fd] = (copy >= 0) ? copy : -2;
}

/* Applies redirections to this process. With a FdSave the originals are
 * kept for restore_redirs(); a forked child passes NULL. Returns -1 if
 * a file cannot be opened (message printed): the command must not run. */
static int apply_redirs(Redir *r, FdSave *save) {
    if (save != NULL) {
        for (int i = 0; i < 10; i++) save->saved[i] = -1;
    }
    if (r != NULL) {
        fflush(stdout); /* buffered output belongs to the old stdout */
    }
    for (; r != NULL; r = r->next) {
        char *target = expand_plain(r->target);
        if (r->kind == R_DUP) {
            save_fd(save, r->fd);
            if (strcmp(target, "-") == 0) {
                close(r->fd);
            } else {
                char *end;
                long from = strtol(target, &end, 10);
                if (*target == '\0' || *end != '\0' || from < 0 || from > 9 ||
                    dup2((int)from, r->fd) < 0) {
                    fprintf(stderr, "acsh: %s: bad file descriptor\n", target);
                    free(target);
                    return -1;
                }
            }
            free(target);
            continue;
        }
        int flags = O_RDONLY;
        switch (r->kind) {
        case R_IN:     flags = O_RDONLY; break;
        case R_OUT:    flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        case R_APPEND: flags = O_WRONLY | O_CREAT | O_APPEND; break;
        case R_RDWR:   flags = O_RDWR | O_CREAT; break;
        case R_DUP:    break;
        }
        int fd = open(target, flags, 0666);
        if (fd < 0) {
            fprintf(stderr, "acsh: %s: %s\n", target, strerror(errno));
            free(target);
            return -1;
        }
        free(target);
        if (fd != r->fd) {
            save_fd(save, r->fd);
            dup2(fd, r->fd);
            close(fd);
        }
    }
    return 0;
}

static void restore_redirs(FdSave *s) {
    fflush(stdout);
    for (int fd = 0; fd < 10; fd++) {
        if (s->saved[fd] >= 0) {
            dup2(s->saved[fd], fd);
            close(s->saved[fd]);
        } else if (s->saved[fd] == -2) {
            close(fd);
        }
    }
}

/* ---- running commands ---- */

static int run_simple(Node *n, int in_child, const char *text);
static int exec_compound(Node *n);

/* execvp() and its error, in a forked child. */
static void exec_external_child(char **argv) {
    execvp(argv[0], argv);
    int err = errno;
    if (err == ENOENT) {
        fprintf(stderr, "acsh: %s: command not found\n", argv[0]);
        child_exit(127);
    }
    if (err == EACCES) {
        fprintf(stderr, "acsh: %s: permission denied (found, but not executable -- "
                "check file permissions or a missing #!/.../interpreter line)\n", argv[0]);
    } else if (err == ENOEXEC) {
        fprintf(stderr, "acsh: %s: cannot execute (not a valid executable -- "
                "if this is a script, it may be missing its #!/bin/sh line)\n", argv[0]);
    } else {
        fprintf(stderr, "acsh: %s: cannot execute: %s\n", argv[0], strerror(err));
    }
    child_exit(126);
}

/* One process of a job: either a node to run in the child, or an
 * external command whose words were already expanded by the shell. */
typedef struct {
    Node  *node;
    char **argv;     /* pre-expanded external command, or NULL */
    char **assigns;  /* its NAME=value prefixes, already expanded */
    Redir *redirs;   /* its redirections (expanded in the child) */
} Stage;

static void run_stage_in_child(Stage *st) {
    if (st->argv != NULL) {
        for (char **a = st->assigns; a != NULL && *a != NULL; a++) {
            char *eq = strchr(*a, '=');
            *eq = '\0';
            setenv(*a, eq + 1, 1);
            *eq = '=';
        }
        if (apply_redirs(st->redirs, NULL) != 0) child_exit(1);
        exec_external_child(st->argv);
    }
    Node *n = st->node;
    if (n->kind == N_SIMPLE) {
        child_exit(run_simple(n, 1, NULL));
    }
    if (n->kind == N_SUBSHELL) {
        if (apply_redirs(n->redirs, NULL) != 0) child_exit(1);
        child_exit(exec_node(n->u.sub.body));
    }
    child_exit(exec_node(n));
}

/* Forks every stage, connects them with pipes and, unless background,
 * waits. Returns the last stage's status. */
static int run_job(Stage *stages, int n, const char *text, int background) {
    pid_t *pids = calloc((size_t)n, sizeof(pid_t));
    if (pids == NULL) {
        fprintf(stderr, "acsh: out of memory\n");
        return 1;
    }
    int jc = job_control;
    /* POSIX: in a non-interactive shell a background job's stdin is
     * /dev/null unless it is redirected */
    int stdin_null = background && !shell_is_interactive();
    pid_t pgid = 0;
    int prev_read = -1;

    /* a script never reaches a prompt, so free finished jobs here or the
     * job table fills up */
    if (background && !shell_is_interactive()) {
        jobs_report_changes();
    }

    /* SIGCHLD stays blocked from before the first fork until the wait
     * is over, so the async handler cannot reap a child (and lose its
     * status) before we wait for it. */
    block_sigchld();

    for (int i = 0; i < n; i++) {
        int pipefd[2] = { -1, -1 };
        if (i < n - 1 && pipe(pipefd) < 0) {
            perror("acsh: pipe");
            break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("acsh: fork");
            if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }
            break;
        }
        if (pid == 0) {
            child_setup_subshell();
            if (jc) {
                setpgid(0, pgid != 0 ? pgid : getpid());
            }
            if (prev_read >= 0) {
                dup2(prev_read, STDIN_FILENO);
                close(prev_read);
            } else if (stdin_null) {
                int devnull = open("/dev/null", O_RDONLY);
                if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
            }
            if (pipefd[1] >= 0) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }
            run_stage_in_child(&stages[i]);
        }
        if (jc) {
            if (pgid == 0) pgid = pid;
            setpgid(pid, pgid); /* also in the parent: no race on who runs first */
        }
        pids[i] = pid;
        if (prev_read >= 0) close(prev_read);
        if (pipefd[1] >= 0) {
            close(pipefd[1]);
            prev_read = pipefd[0];
        }
    }
    if (prev_read >= 0) close(prev_read);

    int started = 0;
    while (started < n && pids[started] != 0) started++;
    if (started == 0) {
        unblock_sigchld();
        free(pids);
        return 1;
    }
    if (pgid == 0) pgid = pids[0];

    if (background) {
        int id = jobs_add(pgid, pids, started, text != NULL ? text : "");
        if (shell_is_interactive() && id > 0) {
            printf("[%d] %d\n", id, (int)pgid);
        }
        unblock_sigchld();
        free(pids);
        return 0;
    }

    if (jc) set_foreground_pgid(pgid);
    int last = 0;
    int remaining = started;
    while (remaining > 0) {
        int status;
        pid_t w = jc ? waitpid(-pgid, &status, WUNTRACED) : waitpid(pids[started - remaining], &status, 0);
        if (w < 0) {
            break;
        }
        if (WIFSTOPPED(status)) {
            /* Ctrl+Z: becomes a stopped job; the prompt comes back */
            jobs_add(pgid, pids, started, text != NULL ? text : "");
            jobs_mark_stopped(pgid);
            last = 128 + WSTOPSIG(status);
            break;
        }
        if (w == pids[started - 1]) {
            last = status_to_exit_code(status);
        }
        if (WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) {
            shell_note_sigint(); /* Ctrl+C also stops an enclosing loop */
        }
        remaining--;
    }
    if (jc) set_foreground_pgid(0);
    unblock_sigchld();
    free(pids);
    return last;
}

int exec_external(char **argv) {
    Stage st = { NULL, argv, NULL, NULL };
    return run_job(&st, 1, argv[0], 0);
}

/* A simple command. In a forked child (in_child) an external command is
 * exec'd directly; otherwise builtins and functions run in the shell
 * itself and external commands are forked as a job (`text` is its
 * source text for `jobs`). */
static int run_simple(Node *n, int in_child, const char *text) {
    Args args = { NULL, 0, 0 };
    int status = 0;

    if (n->u.simple.nwords == 0) {
        /* assignments and/or redirections only: the status is that of
         * the last command substitution, or 0 */
        env_set_last_status(0);
        for (int i = 0; i < n->u.simple.nassigns; i++) {
            set_assignment(n->u.simple.assigns[i]);
        }
        status = env_get_last_status();
        FdSave save;
        if (apply_redirs(n->redirs, in_child ? NULL : &save) != 0) {
            if (!in_child) restore_redirs(&save);
            return 1;
        }
        if (!in_child) restore_redirs(&save);
        return status;
    }

    expand_words(n->u.simple.words, n->u.simple.nwords, 0, &args);
    if (args.n == 0) {
        /* every word expanded to nothing: like an assignment-only line */
        for (int i = 0; i < n->u.simple.nassigns; i++) set_assignment(n->u.simple.assigns[i]);
        args_free(&args);
        return env_get_last_status();
    }

    const char *name = args.v[0];
    int is_func = function_is_defined(name);
    int is_builtin = !is_func && is_builtin_name(name);

    if (is_func || is_builtin) {
        /* assignments before a builtin or function stay set afterwards */
        for (int i = 0; i < n->u.simple.nassigns; i++) set_assignment(n->u.simple.assigns[i]);
        if (is_builtin && strcmp(name, "exec") == 0 && args.n == 1) {
            /* bare `exec 3<file`: the redirections stay in place */
            status = apply_redirs(n->redirs, NULL) != 0 ? 1 : 0;
            args_free(&args);
            return status;
        }
        FdSave save;
        if (apply_redirs(n->redirs, in_child ? NULL : &save) != 0) {
            if (!in_child) restore_redirs(&save);
            args_free(&args);
            return 1;
        }
        if (is_func) {
            status = function_call(args.n, args.v);
        } else {
            Command cmd = { args.v, args.n, args.cap };
            try_run_builtin(&cmd, &status);
        }
        if (!in_child) restore_redirs(&save);
        args_free(&args);
        return status;
    }

    /* external command: its prefix assignments apply to it alone */
    Args assigns = { NULL, 0, 0 };
    for (int i = 0; i < n->u.simple.nassigns; i++) {
        const char *a = n->u.simple.assigns[i];
        const char *eq = strchr(a, '=');
        char *value = expand_plain(eq + 1);
        size_t len = (size_t)(eq - a) + 1 + strlen(value) + 1;
        char *pair = malloc(len);
        snprintf(pair, len, "%.*s=%s", (int)(eq - a), a, value);
        free(value);
        args_push(&assigns, pair);
    }
    Stage st = { n, args.v, assigns.v, n->redirs };
    if (in_child) {
        run_stage_in_child(&st); /* does not return */
    }
    status = run_job(&st, 1, text != NULL ? text : args.v[0], 0);
    args_free(&assigns);
    args_free(&args);
    return status;
}

static int run_pipeline(Node *n) {
    trap_check_and_run_int(); /* a queued INT trap runs between commands */
    if (control_flow_unwinding()) {
        return env_get_last_status();
    }
    int status;
    Node *only = n->u.pipe.cmds[0];
    if (n->u.pipe.ncmds == 1 && only->kind != N_SUBSHELL) {
        status = (only->kind == N_SIMPLE) ? run_simple(only, 0, n->text) : exec_compound(only);
    } else {
        Stage *stages = calloc((size_t)n->u.pipe.ncmds, sizeof(Stage));
        for (int i = 0; i < n->u.pipe.ncmds; i++) {
            stages[i].node = n->u.pipe.cmds[i];
        }
        status = run_job(stages, n->u.pipe.ncmds, n->text, 0);
        free(stages);
    }
    if (n->u.pipe.bang) {
        status = (status == 0) ? 1 : 0;
    }
    env_set_last_status(status);
    return status;
}

static int run_background(Node *n) {
    Node *body = n->u.sub.body;
    Stage *stages;
    int count;
    if (body->kind == N_PIPELINE && !body->u.pipe.bang) {
        /* a plain pipeline runs as the job itself */
        count = body->u.pipe.ncmds;
        stages = calloc((size_t)count, sizeof(Stage));
        for (int i = 0; i < count; i++) stages[i].node = body->u.pipe.cmds[i];
    } else {
        /* anything else (a && b, loops, ...) runs in a forked shell */
        count = 1;
        stages = calloc(1, sizeof(Stage));
        stages[0].node = body;
    }
    run_job(stages, count, n->text, 1);
    free(stages);
    env_set_last_status(0);
    return 0;
}

/* ---- compound commands ---- */

static int run_for(Node *n) {
    Args items = { NULL, 0, 0 };
    if (n->u.forl.has_in) {
        expand_words(n->u.forl.words, n->u.forl.nwords, 1, &items);
    } else {
        for (int i = 1; i <= env_positional_count(); i++) {
            args_push(&items, strdup(env_positional(i)));
        }
    }
    int status = 0;
    loop_depth++;
    for (int i = 0; i < items.n; i++) {
        setenv(n->u.forl.var, items.v[i], 1);
        status = exec_node(n->u.forl.body);
        if (loop_check() == LEAVE) break;
    }
    loop_depth--;
    args_free(&items);
    return status;
}

static int run_loop(Node *n) {
    int status = 0;
    loop_depth++;
    for (;;) {
        int cond = exec_node(n->u.loop.cond);
        int what = loop_check();
        if (what == LEAVE) break;
        if (what == NEXT) continue;
        if (n->kind == N_WHILE ? cond != 0 : cond == 0) break;
        status = exec_node(n->u.loop.body);
        if (loop_check() == LEAVE) break;
    }
    loop_depth--;
    return status;
}

static int run_case(Node *n) {
    char *subject = expand_plain(n->u.cases.subject);
    int status = 0;
    for (CaseItem *it = n->u.cases.items; it != NULL; it = it->next) {
        for (int i = 0; i < it->npatterns; i++) {
            char *pat = case_pattern(it->patterns[i]);
            int match = fnmatch(pat, subject, 0) == 0;
            free(pat);
            if (match) {
                status = exec_node(it->body);
                free(subject);
                return status;
            }
        }
    }
    free(subject);
    return status;
}

/* A compound command or function definition, with its redirections. */
static int exec_compound(Node *n) {
    if (n->kind == N_FUNCDEF) {
        function_define(n->u.func.name, node_ref(n->u.func.body));
        return 0;
    }
    FdSave save;
    if (apply_redirs(n->redirs, &save) != 0) {
        restore_redirs(&save);
        return 1;
    }
    int status = 0;
    switch (n->kind) {
    case N_GROUP:    status = exec_node(n->u.sub.body); break;
    case N_IF: {
        int cond = exec_node(n->u.ifs.cond);
        if (control_flow_unwinding()) { status = cond; break; }
        if (cond == 0) status = exec_node(n->u.ifs.then_part);
        else if (n->u.ifs.else_part != NULL) status = exec_node(n->u.ifs.else_part);
        break;
    }
    case N_WHILE: case N_UNTIL: status = run_loop(n); break;
    case N_FOR:      status = run_for(n); break;
    case N_CASE:     status = run_case(n); break;
    default:         status = exec_node(n); break;
    }
    restore_redirs(&save);
    env_set_last_status(status);
    return status;
}

int exec_node(Node *n) {
    if (n == NULL) {
        return 0;
    }
    int status;
    switch (n->kind) {
    case N_SEQ:
        status = exec_node(n->u.bin.left);
        if (control_flow_unwinding()) return status;
        return exec_node(n->u.bin.right);
    case N_AND:
    case N_OR:
        status = exec_node(n->u.bin.left);
        if (control_flow_unwinding()) return status;
        if ((n->kind == N_AND) == (status == 0)) {
            status = exec_node(n->u.bin.right);
        }
        return status;
    case N_BACKGROUND:
        return run_background(n);
    case N_PIPELINE:
        return run_pipeline(n);
    case N_SIMPLE:
        status = run_simple(n, 0, NULL);
        env_set_last_status(status);
        return status;
    case N_SUBSHELL: {
        Stage st = { n, NULL, NULL, NULL };
        status = run_job(&st, 1, n->text, 0);
        env_set_last_status(status);
        return status;
    }
    default:
        return exec_compound(n);
    }
}

/* ---- running text ---- */

int run_lexer(Lexer *lx) {
    for (;;) {
        Node *n;
        ParseResult r = parse_next(lx, &n);
        if (r == PARSE_EOF) {
            return 0;
        }
        if (r == PARSE_ERROR) {
            env_set_last_status(2);
            return -1;
        }
        if (n != NULL) {
            exec_node(n);
            node_unref(n);
        }
        trap_check_and_run_int();
        if (shell_interrupted()) {
            return -2;
        }
    }
}

int run_string(const char *text) {
    Lexer *lx = lexer_new(text, NULL, NULL, NULL);
    int r = run_lexer(lx);
    lexer_free(lx);
    return r == -1 ? 2 : env_get_last_status();
}
