#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "acsh.h"

/* Runs `cmd` as a shell command line in a forked SUBSHELL and returns
 * everything it wrote to stdout as a newly malloc'd string (caller
 * frees), with trailing newlines stripped -- exactly POSIX's command
 * substitution semantics. Returns an empty string (never NULL, unless
 * out of memory) if the command produced no output or couldn't run.
 *
 * Why a forked child rather than running it in-process: POSIX runs a
 * substitution in a subshell, so side effects (variable assignments,
 * cd, function definitions) made inside $(...) must NOT leak into the
 * parent shell. fork() gives that isolation for free, and it also
 * lets us capture stdout simply by pointing the child's fd 1 at a
 * pipe -- no need to redirect anything inside the parent. */
char *command_substitute(const char *cmd) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("acsh: pipe");
        return strdup("");
    }

    /* Block SIGCHLD around this function's own fork()/waitpid(), for
     * exactly the same reason execute_pipeline() blocks it around ITS
     * fork()/waitpid() (see the long comment at that call site in
     * executor.c). The main shell process has an installed async
     * SIGCHLD handler (signals.c) that reaps any of its children via
     * a WNOHANG loop whenever the signal arrives. The child forked
     * below often exits almost immediately (e.g. `$(false)`), so
     * without blocking here, that async handler can reap this
     * function's own direct child BEFORE the explicit waitpid() call
     * further down ever runs -- making that waitpid() fail with
     * ECHILD and silently leaving this function unable to observe the
     * real exit status. This is a SEPARATE instance of the same race
     * already fixed once in execute_pipeline(): that fix protects
     * execute_pipeline()'s own fork/wait pairs, but does nothing for
     * THIS function's outer fork/wait pair, which runs one level
     * further out (this function forks a subshell that itself calls
     * execute_pipeline() for whatever command is inside the
     * substitution -- two independent fork/wait pairs, two
     * independent places this exact race can happen). */
    block_sigchld();

    pid_t pid = fork();
    if (pid < 0) {
        perror("acsh: fork");
        close(pipefd[0]);
        close(pipefd[1]);
        unblock_sigchld();
        return strdup("");
    }

    if (pid == 0) {
        /* ---- child (the subshell) ---- */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        /* The child inherited the parent's signal HANDLER for
         * SIGCHLD, so restore it to default here -- and also inherited
         * SIGCHLD being BLOCKED (block_sigchld() above, before this
         * fork()), which must be undone too: this subshell is about
         * to call execute_chain()/execute_pipeline() for whatever
         * command is inside the substitution, and THAT code manages
         * its own SIGCHLD blocking around its own fork/wait pair (see
         * executor.c) -- it needs to start from an unblocked mask, the
         * same as any freshly-started command execution would. */
        signal(SIGCHLD, SIG_DFL);
        unblock_sigchld();

        char *text = strdup(cmd);
        if (text == NULL) {
            _exit(1);
        }

        /* Normalize newlines to ';' so a multi-line substitution body
         * runs as several statements, matching what control_flow.c
         * does for multi-line construct bodies. */
        for (char *q = text; *q != '\0'; q++) {
            if (*q == '\n') *q = ';';
        }

        Chain ch;
        if (parse_chain(text, &ch) == 0 && ch.num_segments > 0) {
            execute_chain(&ch);
        }
        free(text);
        fflush(stdout);
        _exit(env_get_last_status() & 0xff);
    }

    /* ---- parent ---- */
    close(pipefd[1]);

    size_t cap = 256;
    size_t len = 0;
    char *out = malloc(cap);
    if (out == NULL) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        unblock_sigchld();
        return NULL;
    }

    /* Read until EOF. The child's stdout is the write end of this
     * pipe, so EOF arrives exactly when the child (and anything it
     * spawned that inherited fd 1) has closed it. Reading BEFORE
     * waitpid() matters: if we waited first and the child wrote more
     * than a pipe buffer's worth (~64KB), it would block forever on
     * a full pipe while we blocked forever waiting for it to exit --
     * a classic deadlock. */
    while (1) {
        if (len + 128 >= cap) {
            cap *= 2;
            char *bigger = realloc(out, cap);
            if (bigger == NULL) {
                free(out);
                close(pipefd[0]);
                waitpid(pid, NULL, 0);
                unblock_sigchld();
                return NULL;
            }
            out = bigger;
        }
        ssize_t n = read(pipefd[0], out + len, cap - len - 1);
        if (n < 0) {
            break;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        env_set_last_status(WEXITSTATUS(status));
    }
    unblock_sigchld();

    /* POSIX: strip ALL trailing newlines from the captured output
     * (but leave embedded ones alone) */
    while (len > 0 && out[len - 1] == '\n') {
        len--;
    }
    out[len] = '\0';
    return out;
}
