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

    /* SIGCHLD stays blocked from before the fork until our waitpid():
     * otherwise the async handler (signals.c) can reap a fast child,
     * e.g. $(false), first, and its exit status is lost. The same rule
     * as for every job in exec.c. */
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

        /* the subshell: a forked copy of this shell runs the text and
         * exits with its status; nothing it changes reaches the parent */
        child_setup_subshell();
        int status = run_string(cmd);
        fflush(stdout);
        _exit(status & 0xff);
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
