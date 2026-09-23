#define _POSIX_C_SOURCE 200809L  /* sigaction, kill, getpgid under -std=c11 */
#include <stdio.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "acsh.h"

/* The shell needs to know which process group is currently running in
 * the foreground so that Ctrl+C/Ctrl+Z can be routed to it instead of
 * killing the shell itself. executor.c sets this before waiting on a
 * foreground pipeline, and clears it (back to 0) once it's done. */
static volatile pid_t foreground_pgid = 0;

void set_foreground_pgid(pid_t pgid) {
    foreground_pgid = pgid;
}

/* SIGCHLD fires whenever ANY child changes state (exits, is killed, or
 * is stopped). We reap every child we can with a non-blocking
 * waitpid(-1, ..., WNOHANG) loop so zombies never pile up, and update
 * the job table for any pgid that finished or stopped in the
 * background while we weren't explicitly waiting on it. */
static void sigchld_handler(int signo) {
    (void)signo;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        pid_t pgid = getpgid(pid);
        if (pgid < 0) {
            /* process already fully gone; getpgid can fail here, use
             * the pid itself as a best-effort fallback so jobs_mark_*
             * still has something to match against. */
            pgid = pid;
        }

        if (WIFSTOPPED(status)) {
            jobs_mark_stopped(pgid);
        } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
            jobs_mark_done(pgid);
        }
    }
}

/* Ctrl+C: forward SIGINT to the foreground job's process group instead
 * of letting it kill the shell itself. If nothing is in the
 * foreground, do nothing (matches how bash behaves at an empty prompt). */
static void sigint_handler(int signo) {
    if (foreground_pgid > 0) {
        kill(-foreground_pgid, signo);
    }
    /* if foreground_pgid == 0, we're at the prompt -- ignore, like bash */
}

/* Ctrl+Z: same idea, forward SIGTSTP to the foreground job. */
static void sigtstp_handler(int signo) {
    if (foreground_pgid > 0) {
        kill(-foreground_pgid, signo);
    }
}

void signals_init(void) {
    struct sigaction sa_chld;
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_int;
    sa_int.sa_handler = sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa_int, NULL);

    struct sigaction sa_tstp;
    sa_tstp.sa_handler = sigtstp_handler;
    sigemptyset(&sa_tstp.sa_mask);
    sa_tstp.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &sa_tstp, NULL);
}
