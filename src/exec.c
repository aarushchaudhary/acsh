#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include "acsh.h"
#include "syntax.h"
#include "lex.h"   /* the lexer's source name */

extern char **environ;

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

/* Files being read (scripts, `.`, profiles), nested. Inside one, a
 * `return` outside any function ends that file, as in ash. */
static int file_depth = 0;

void control_flow_enter_file(void) {
    file_depth++;
}

void control_flow_leave_file(void) {
    file_depth--;
}

int control_flow_in_file(void) {
    return file_depth > 0;
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

/* Job control is on in an interactive shell only; never in a forked
 * child, where the parent shell owns the job. */
static int job_control = 0;
static int tty_fd = -1;                 /* the controlling terminal, if any */
static pid_t shell_pgid = 0;
static struct termios shell_tmodes;

int job_control_enabled(void) {
    return job_control;
}

void job_control_init(void) {
    job_control = 1;
    shell_set_option('m', 1);
    if (!isatty(STDIN_FILENO)) {
        return; /* -i without a terminal: jobs, but no terminal to hand over */
    }
    int fd = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 10);
    if (fd < 0) {
        return;
    }
    /* wait until we are in the foreground, as ash does */
    pid_t fg;
    while ((fg = tcgetpgrp(fd)) > 0 && fg != getpgrp()) {
        kill(-getpgrp(), SIGTTIN);
    }
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    shell_pgid = getpid();
    if (getpgrp() != shell_pgid) {
        setpgid(0, shell_pgid);
    }
    shell_pgid = getpgrp();
    tcsetpgrp(fd, shell_pgid);
    tcgetattr(fd, &shell_tmodes);
    tty_fd = fd;
}

void terminal_give(Job *j, pid_t pgid) {
    if (!job_control || tty_fd < 0) return;
    if (j != NULL && j->has_tmodes) {
        tcsetattr(tty_fd, TCSADRAIN, &j->tmodes);
    }
    tcsetpgrp(tty_fd, pgid);
}

void terminal_take_back(Job *stopped) {
    if (!job_control || tty_fd < 0) return;
    tcsetpgrp(tty_fd, shell_pgid);
    if (stopped != NULL) {
        stopped->has_tmodes = (tcgetattr(tty_fd, &stopped->tmodes) == 0);
    }
    tcsetattr(tty_fd, TCSADRAIN, &shell_tmodes); /* e.g. after vi was stopped in raw mode */
}

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
    /* trap '' INT / TSTP is inherited (POSIX); otherwise back to default */
    if (!trap_is_ignored(SIGINT)) signal_default(SIGINT);
    if (!trap_is_ignored(SIGTSTP)) signal_default(SIGTSTP);
    signal(SIGCHLD, SIG_DFL);
    /* signals an interactive shell ignores go back to their defaults,
     * unless the user ignored them with trap '' */
    int ignored_by_shell[] = { SIGTTOU, SIGTTIN, SIGQUIT, SIGTERM };
    for (int i = 0; i < 4; i++) {
        if (!trap_is_ignored(ignored_by_shell[i])) signal_default(ignored_by_shell[i]);
    }
    unblock_sigchld();
    job_control = 0;
    tty_fd = -1;
    shell_mark_subshell();      /* not interactive: no prompts or job notices */
    trap_reset_for_subshell();  /* traps are not inherited by subshells */
}

static void child_exit(int status) {
    /* only the output streams: flushing an input stream would move the
     * file offset the parent shell is still reading its script from */
    fflush(stdout);
    fflush(stderr);
    _exit(status & 0xff);
}

/* ---- expansion ---- */

typedef struct {
    char **v;
    int    n, cap;
} Args;

static void args_push(Args *a, char *s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->v = realloc(a->v, (size_t)a->cap * sizeof(char *));
        if (a->v == NULL) {
            shell_error("out of memory\n");
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

/* An expansion error (${x?}, $((1/0)), bad substitution) was reported:
 * as in ash, a non-interactive shell exits with status 2; at the prompt
 * only the command fails. */
static int expansion_failed(void) {
    if (!shell_is_interactive()) {
        trap_run_exit();
        exit(2);
    }
    return 2;
}

/* Words -> arguments (split, globbed). Returns -1 on an expansion error. */
static int expand_args(char **words, int nwords, Args *out) {
    char **fields;
    int n;
    if (expand_fields(words, nwords, &fields, &n) != 0) {
        free(fields);
        return -1;
    }
    for (int i = 0; i < n; i++) args_push(out, fields[i]);
    free(fields);
    return 0;
}

/* NAME=value. Returns -1 on an expansion error. */
static int set_assignment(const char *assign) {
    const char *eq = strchr(assign, '=');
    char *value = expand_string(eq + 1, 1);
    if (value == NULL) {
        return -1;
    }
    char *name = strndup(assign, (size_t)(eq - assign));
    int r = var_set(name, value);
    free(name);
    free(value);
    return r;
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

/* A here-document's (expanded) body as a readable fd: an unlinked
 * temporary file, so a body of any size cannot block the shell the way
 * a full pipe would. */
static int heredoc_fd(const char *body) {
    const char *dir = var_get("TMPDIR");
    char path[4096];
    snprintf(path, sizeof(path), "%s/acsh-heredoc-XXXXXX", (dir != NULL && *dir) ? dir : "/tmp");
    int fd = mkstemp(path);
    if (fd < 0) {
        shell_error("here-document: %s\n", strerror(errno));
        return -1;
    }
    unlink(path);
    size_t len = strlen(body), done = 0;
    while (done < len) {
        ssize_t n = write(fd, body + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            shell_error("here-document: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
        done += (size_t)n;
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
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
        char *target = expand_string(r->target, 0);
        if (target == NULL) {
            return -1;
        }
        if (r->kind == R_HEREDOC) {
            int fd = heredoc_fd(target);
            free(target);
            if (fd < 0) {
                return -1;
            }
            save_fd(save, r->fd);
            dup2(fd, r->fd);
            if (fd != r->fd) close(fd);
            continue;
        }
        if (r->kind == R_DUP) {
            save_fd(save, r->fd);
            if (strcmp(target, "-") == 0) {
                close(r->fd);
            } else {
                char *end;
                long from = strtol(target, &end, 10);
                if (*target == '\0' || *end != '\0' || from < 0 || from > 9 ||
                    dup2((int)from, r->fd) < 0) {
                    shell_error("%s: bad file descriptor\n", target);
                    free(target);
                    return -1;
                }
            }
            free(target);
            continue;
        }
        int flags = O_RDONLY;
        struct stat st;
        if (r->kind == R_OUT && shell_option('C') && stat(target, &st) == 0 && S_ISREG(st.st_mode)) {
            /* set -C: `>` does not overwrite an existing file; `>|` does */
            shell_error("%s: file exists\n", target);
            free(target);
            return -1;
        }
        switch (r->kind) {
        case R_HEREDOC: break; /* handled above */
        case R_IN:     flags = O_RDONLY; break;
        case R_OUT: case R_CLOBBER: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        case R_APPEND: flags = O_WRONLY | O_CREAT | O_APPEND; break;
        case R_RDWR:   flags = O_RDWR | O_CREAT; break;
        case R_DUP:    break;
        }
        int fd = open(target, flags, 0666);
        if (fd < 0) {
            shell_error("%s: %s\n", target, strerror(errno));
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

/* ---- set -e and set -x ---- */

/* > 0 while running a context where set -e does not apply: an if/while
 * condition, the left side of && or ||, a ! pipeline (POSIX). */
static int errexit_off = 0;

/* After a command: a failure (outside the contexts above) runs the
 * ERR trap, then ends the shell under set -e. */
static void check_errexit(int status) {
    if (status == 0 || errexit_off > 0) {
        return;
    }
    trap_run_err();
    if (shell_option('e')) {
        trap_run_exit();
        exit(status);
    }
}

/* Writes s for a set -x trace, single-quoted when it is not plain. */
static void trace_word(const char *s) {
    int plain = *s != '\0';
    for (const char *c = s; *c; c++) {
        if (!(isalnum((unsigned char)*c) || strchr("_./:=+,%@^-", *c))) plain = 0;
    }
    if (plain) {
        fputs(s, stderr);
        return;
    }
    fputc('\'', stderr);
    for (const char *c = s; *c; c++) {
        if (*c == '\'') fputs("'\"'\"'", stderr);
        else fputc(*c, stderr);
    }
    fputc('\'', stderr);
}

/* set -x: "+ NAME=value... word..." on stderr before a command runs. */
static void trace(char **assigns, int nassigns, int from_vars, char **words, int nwords) {
    if (!shell_option('x')) return;
    const char *ps4 = var_get("PS4");
    fputs(ps4 != NULL ? ps4 : "+ ", stderr);
    int first = 1;
    for (int i = 0; i < nassigns; i++) {
        const char *eq = strchr(assigns[i], '=');
        if (!first) fputc(' ', stderr);
        first = 0;
        fprintf(stderr, "%.*s=", (int)(eq - assigns[i]), assigns[i]);
        if (from_vars) {
            char *name = strndup(assigns[i], (size_t)(eq - assigns[i]));
            const char *v = var_get(name);
            trace_word(v != NULL ? v : "");
            free(name);
        } else {
            trace_word(eq + 1);
        }
    }
    for (int i = 0; i < nwords; i++) {
        if (!first) fputc(' ', stderr);
        first = 0;
        trace_word(words[i]);
    }
    fputc('\n', stderr);
}

/* ---- running commands ---- */

static int run_simple(Node *n, int in_child, const char *text);
static int exec_compound(Node *n);

/* execvp() and its error, in a forked child. */
static void exec_external_child(char **argv) {
    char **env = var_environ();
    if (env != NULL) environ = env;  /* exported variables only */
    execvp(argv[0], argv);
    int err = errno;
    if (err == ENOENT) {
        shell_error("%s: command not found\n", argv[0]);
        child_exit(127);
    }
    if (err == EACCES) {
        shell_error("%s: permission denied (found, but not executable -- "
                "check file permissions or a missing #!/.../interpreter line)\n", argv[0]);
    } else if (err == ENOEXEC) {
        shell_error("%s: cannot execute (not a valid executable -- "
                "if this is a script, it may be missing its #!/bin/sh line)\n", argv[0]);
    } else {
        shell_error("%s: cannot execute: %s\n", argv[0], strerror(err));
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
        /* prefix assignments are exported to the command they precede */
        for (char **a = st->assigns; a != NULL && *a != NULL; a++) {
            char *eq = strchr(*a, '=');
            *eq = '\0';
            if (var_set(*a, eq + 1) != 0) child_exit(2);
            var_add_flags(*a, V_EXPORT);
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
        shell_error("out of memory\n");
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
            int tty = tty_fd;
            child_setup_subshell();
            if (background && !jc) {
                /* POSIX: without job control, an asynchronous command
                 * ignores SIGINT and SIGQUIT */
                signal_lock_ignored(SIGINT);
                signal_lock_ignored(SIGQUIT);
            }
            if (jc) {
                pid_t group = (pgid != 0) ? pgid : getpid();
                setpgid(0, group);
                if (!background && tty >= 0) {
                    /* take the terminal ourselves too: we may exec and
                     * read it before the parent gets to tcsetpgrp() */
                    signal(SIGTTOU, SIG_IGN);
                    tcsetpgrp(tty, group);
                    signal(SIGTTOU, SIG_DFL);
                }
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
        if (id > 0) jobs_find_by_id(id)->own_group = jc;
        env_set_last_background(pids[started - 1]);
        if (shell_is_interactive() && id > 0) {
            fprintf(stderr, "[%d] %d\n", id, (int)pgid);
        }
        unblock_sigchld();
        free(pids);
        return 0;
    }

    if (jc) {
        set_foreground_pgid(pgid);
        terminal_give(NULL, pgid);
    }
    int last = 0;
    int stopped = 0;
    int failed_status = 0;   /* set -o pipefail: the rightmost failure */
    int failed_index = -1;
    int remaining = started;
    while (remaining > 0) {
        int status;
        pid_t w = jc ? waitpid(-pgid, &status, WUNTRACED) : waitpid(pids[started - remaining], &status, 0);
        if (w < 0) {
            if (errno == EINTR) continue; /* a trapped signal: its trap runs afterwards */
            break;
        }
        if (WIFSTOPPED(status)) {
            /* Ctrl+Z: becomes a stopped job; the prompt comes back */
            int id = jobs_add(pgid, pids, started, text != NULL ? text : "");
            Job *j = (id > 0) ? jobs_find_by_id(id) : NULL;
            if (j != NULL) j->own_group = 1;
            jobs_mark_stopped(pgid);
            terminal_take_back(j);
            last = 128 + WSTOPSIG(status);
            stopped = 1;
            break;
        }
        if (w == pids[started - 1]) {
            last = status_to_exit_code(status);
            int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
            if (sig != 0 && sig != SIGINT && sig != SIGPIPE) {
                fprintf(stderr, "%s\n", strsignal(sig)); /* "Killed", as ash reports */
            }
        }
        for (int k = 0; k < started; k++) {
            if (pids[k] == w && status_to_exit_code(status) != 0 && k > failed_index) {
                failed_index = k;
                failed_status = status_to_exit_code(status);
            }
        }
        if (WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) {
            shell_note_sigint(); /* Ctrl+C also stops an enclosing loop */
        }
        remaining--;
    }
    if (jc) {
        set_foreground_pgid(0);
        if (!stopped) terminal_take_back(NULL);
    }
    unblock_sigchld();
    free(pids);
    if (shell_option('P') && last == 0 && failed_index >= 0) {
        return failed_status;
    }
    return last;
}

int exec_external(char **argv) {
    Stage st = { NULL, argv, NULL, NULL };
    return run_job(&st, 1, argv[0], 0);
}

/* Runs a builtin or function (redirections applied around it). */
static int run_builtin_or_function(Node *n, Args *args, int is_func, int in_child) {
    const char *name = args->v[0];
    int status = 0;
    if (!is_func && strcmp(name, "exec") == 0 && args->n == 1) {
        /* bare `exec 3<file`: the redirections stay in place */
        return apply_redirs(n->redirs, NULL) != 0 ? 1 : 0;
    }
    FdSave save;
    if (apply_redirs(n->redirs, in_child ? NULL : &save) != 0) {
        if (!in_child) restore_redirs(&save);
        return 1;
    }
    if (is_func) {
        status = function_call(args->n, args->v);
    } else {
        Command cmd = { args->v, args->n, args->cap };
        try_run_builtin(&cmd, &status);
    }
    if (!in_child) restore_redirs(&save);
    return status;
}

/* A simple command. In a forked child (in_child) an external command is
 * exec'd directly; otherwise builtins and functions run in the shell
 * itself and external commands are forked as a job (`text` is its
 * source text for `jobs`). */
static int run_simple(Node *n, int in_child, const char *text) {
    Args args = { NULL, 0, 0 };
    int status = 0;
    env_set_lineno(n->lineno);

    if (n->u.simple.nwords == 0) {
        /* assignments and/or redirections only: the status is that of
         * the last command substitution, or 0 */
        env_set_last_status(0);
        for (int i = 0; i < n->u.simple.nassigns; i++) {
            if (set_assignment(n->u.simple.assigns[i]) != 0) return expansion_failed();
        }
        status = env_get_last_status();
        trace(n->u.simple.assigns, n->u.simple.nassigns, 1, NULL, 0);
        FdSave save;
        if (apply_redirs(n->redirs, in_child ? NULL : &save) != 0) {
            if (!in_child) restore_redirs(&save);
            return 1;
        }
        if (!in_child) restore_redirs(&save);
        return status;
    }

    if (expand_args(n->u.simple.words, n->u.simple.nwords, &args) != 0) {
        args_free(&args);
        return expansion_failed();
    }
    if (args.n == 0) {
        /* every word expanded to nothing: like an assignment-only line */
        for (int i = 0; i < n->u.simple.nassigns; i++) {
            if (set_assignment(n->u.simple.assigns[i]) != 0) return expansion_failed();
        }
        args_free(&args);
        return env_get_last_status();
    }

    const char *name = args.v[0];
    int is_func = function_is_defined(name);
    int is_builtin = !is_func && is_builtin_name(name);

    if (is_func || is_builtin) {
        /* NAME=value before a special builtin stays set afterwards; before
         * a function or another builtin it lasts for that command only */
        int keep = is_builtin && is_special_builtin(name);
        int nsaved = keep ? 0 : n->u.simple.nassigns;
        VarSave *saved = nsaved ? calloc((size_t)nsaved, sizeof(VarSave)) : NULL;
        for (int i = 0; i < n->u.simple.nassigns; i++) {
            if (!keep) {
                const char *a = n->u.simple.assigns[i];
                char *nm = strndup(a, (size_t)(strchr(a, '=') - a));
                saved[i] = var_save(nm);
                free(nm);
            }
            if (set_assignment(n->u.simple.assigns[i]) != 0) {
                for (int k = i; k >= 0 && !keep; k--) var_restore(&saved[k]);
                free(saved);
                args_free(&args);
                return expansion_failed();
            }
        }
        trace(n->u.simple.assigns, n->u.simple.nassigns, 1, args.v, args.n);
        status = run_builtin_or_function(n, &args, is_func, in_child);
        for (int i = nsaved - 1; i >= 0; i--) var_restore(&saved[i]);
        free(saved);
        args_free(&args);
        return status;
    }
    /* external command: its prefix assignments apply to it alone */
    Args assigns = { NULL, 0, 0 };
    for (int i = 0; i < n->u.simple.nassigns; i++) {
        const char *a = n->u.simple.assigns[i];
        const char *eq = strchr(a, '=');
        char *value = expand_string(eq + 1, 1);
        if (value == NULL) {
            args_free(&assigns);
            args_free(&args);
            return expansion_failed();
        }
        size_t len = (size_t)(eq - a) + 1 + strlen(value) + 1;
        char *pair = malloc(len);
        snprintf(pair, len, "%.*s=%s", (int)(eq - a), a, value);
        free(value);
        args_push(&assigns, pair);
    }
    trace(assigns.v, assigns.n, 0, args.v, args.n);
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
    trap_run_pending(); /* queued traps run between commands */
    if (control_flow_unwinding()) {
        return env_get_last_status();
    }
    int status;
    Node *only = n->u.pipe.cmds[0];
    if (n->u.pipe.bang) errexit_off++;
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
        errexit_off--;
        status = (status == 0) ? 1 : 0;
    } else {
        check_errexit(status);
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
        if (expand_args(n->u.forl.words, n->u.forl.nwords, &items) != 0) {
            args_free(&items);
            return expansion_failed();
        }
    } else {
        for (int i = 1; i <= env_positional_count(); i++) {
            args_push(&items, strdup(env_positional(i)));
        }
    }
    int status = 0;
    loop_depth++;
    for (int i = 0; i < items.n; i++) {
        if (var_set(n->u.forl.var, items.v[i]) != 0) {
            status = expansion_failed();
            break;
        }
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
        errexit_off++;
        int cond = exec_node(n->u.loop.cond);
        errexit_off--;
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
    char *subject = expand_string(n->u.cases.subject, 0);
    if (subject == NULL) {
        return expansion_failed();
    }
    int status = 0;
    for (CaseItem *it = n->u.cases.items; it != NULL; it = it->next) {
        for (int i = 0; i < it->npatterns; i++) {
            char *pat = expand_pattern(it->patterns[i]);
            if (pat == NULL) {
                free(subject);
                return expansion_failed();
            }
            int match = fnmatch(pat, subject, 0) == 0;
            free(pat);
            if (match) {
                free(subject);
                status = exec_node(it->body);
                /* ;& runs the next clause's body without testing it */
                while (it->fallthrough && it->next != NULL && !control_flow_unwinding()) {
                    it = it->next;
                    status = exec_node(it->body);
                }
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
        errexit_off++;
        int cond = exec_node(n->u.ifs.cond);
        errexit_off--;
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

static int exec_node_inner(Node *n);

/* Runs n; pipes of <(...) and >(...) it opened close when it is done. */
int exec_node(Node *n) {
    if (shell_option('n') && !shell_is_interactive()) {
        return 0; /* set -n: read commands, run none (ignored when interactive) */
    }
    int mark = process_subst_mark();
    int status = exec_node_inner(n);
    process_close_from(mark);
    return status;
}

static int exec_node_inner(Node *n) {
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
        errexit_off++;
        status = exec_node(n->u.bin.left);
        errexit_off--;
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
        check_errexit(status);
        return status;
    case N_SUBSHELL: {
        Stage st = { n, NULL, NULL, NULL };
        status = run_job(&st, 1, n->text, 0);
        env_set_last_status(status);
        check_errexit(status);
        return status;
    }
    default:
        return exec_compound(n);
    }
}

/* ---- error messages ---- */

/* The script (or "-c") whose commands are running, for error messages;
 * NULL at the interactive prompt. */
static const char *error_source = NULL;

void shell_error_prefix(void) {
    fflush(stdout);
    if (error_source != NULL) {
        fprintf(stderr, "acsh: %s: line %d: ", error_source, env_get_lineno());
    } else {
        fputs("acsh: ", stderr);
    }
}

void shell_error(const char *fmt, ...) {
    va_list ap;
    shell_error_prefix();
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---- running text ---- */

static int run_lexer_inner(Lexer *lx);

int run_lexer(Lexer *lx) {
    const char *saved = error_source;
    if (lx->name != NULL) {
        error_source = lx->name; /* eval and $(...) keep the enclosing file's name */
    }
    int r = run_lexer_inner(lx);
    error_source = saved;
    return r;
}

static int run_lexer_inner(Lexer *lx) {
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
            /* -n: read and check commands without running them (ignored
             * at the interactive prompt, as in ash) */
            if (!shell_option('n') || shell_is_interactive()) {
                exec_node(n);
            }
            node_unref(n);
        }
        trap_run_pending();
        if (shell_interrupted()) {
            return -2;
        }
        if (return_pending && file_depth > 0) {
            return 0; /* `return` at the top of a file ends the file */
        }
    }
}

int run_string(const char *text) {
    Lexer *lx = lexer_new(text, NULL, NULL, NULL);
    int r = run_lexer(lx);
    lexer_free(lx);
    return r == -1 ? 2 : env_get_last_status();
}
