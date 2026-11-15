#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
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
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
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
                while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
                unblock_sigchld();
                return NULL;
            }
            out = bigger;
        }
        ssize_t n = read(pipefd[0], out + len, cap - len - 1);
        if (n < 0) {
            if (errno == EINTR) continue; /* a trapped signal arrived */
            break;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
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

/* ---- process substitution ---- */

/* The shell's ends of <(...) and >(...) pipes, open until the command
 * that used them finishes (process_close_from). */
static int subst_fds[64];
static int subst_count = 0;

int process_substitute(const char *cmd, int to_cmd) {
    int p[2];
    if (subst_count >= (int)(sizeof(subst_fds) / sizeof(subst_fds[0]))) {
        shell_error("too many process substitutions\n");
        return -1;
    }
    if (pipe(p) < 0) {
        shell_error("pipe: %s\n", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        shell_error("fork: %s\n", strerror(errno));
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        /* >(cmd) reads what the command writes; <(cmd) writes for it */
        dup2(to_cmd ? p[0] : p[1], to_cmd ? STDIN_FILENO : STDOUT_FILENO);
        close(p[0]);
        close(p[1]);
        for (int k = 0; k < subst_count; k++) close(subst_fds[k]);
        child_setup_subshell();
        int status = run_string(cmd);
        fflush(stdout);
        _exit(status & 0xff);
    }
    int keep = to_cmd ? p[1] : p[0];
    close(to_cmd ? p[0] : p[1]);
    /* above the 0-9 that redirections use, so `3<file` cannot clobber it */
    int fd = fcntl(keep, F_DUPFD, 60);
    if (fd >= 0) {
        close(keep);
    } else {
        fd = keep;
    }
    subst_fds[subst_count++] = fd;
    return fd;
}

int process_subst_mark(void) {
    return subst_count;
}

void process_close_from(int mark) {
    while (subst_count > mark) close(subst_fds[--subst_count]);
}
