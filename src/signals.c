#define _POSIX_C_SOURCE 200809L  /* sigaction, kill, getpgid under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* trap command storage for INT and EXIT (the two signals/pseudo-
 * signals acsh supports trapping -- see the scope note on
 * builtin_trap() in builtins.c for why the full POSIX trap signal
 * list isn't implemented). The handler itself does NOT run the
 * trapped command directly: running arbitrary shell code (which forks,
 * allocates, etc.) from inside a signal handler is not async-signal-
 * safe and can corrupt the shell's own state if the signal arrives
 * mid-allocation elsewhere. Instead, sigint_handler() just raises a
 * flag (int_trap_pending), and main.c's REPL loop checks and clears
 * that flag BETWEEN commands, running the trapped command from
 * ordinary (non-signal-handler) context where it's safe to do so. */
static char *int_trap_command = NULL;
static volatile sig_atomic_t int_trap_pending = 0;
static char *exit_trap_command = NULL;

void trap_set_int(const char *command) {
    free(int_trap_command);
    int_trap_command = (command != NULL) ? strdup(command) : NULL;
}

void trap_set_exit(const char *command) {
    free(exit_trap_command);
    exit_trap_command = (command != NULL) ? strdup(command) : NULL;
}

/* Called from main.c's REPL loop, between commands, to run the INT
 * trap if one fired since the last check. Returns 1 if it ran a trap
 * (so main.c can re-prompt cleanly), 0 otherwise. */
int trap_check_and_run_int(void) {
    if (!int_trap_pending) {
        return 0;
    }
    int_trap_pending = 0;
    if (int_trap_command != NULL) {
        Chain ch;
        if (parse_chain(int_trap_command, &ch) == 0 && ch.num_segments > 0) {
            execute_chain(&ch);
        }
    }
    return 1;
}

/* Called once from main.c right before the shell exits, to run the
 * EXIT trap if one was set -- this is what lets a script register
 * cleanup code (e.g. removing a temp file) that always runs on exit,
 * regardless of how the script got there. */
void trap_run_exit(void) {
    if (exit_trap_command != NULL) {
        Chain ch;
        if (parse_chain(exit_trap_command, &ch) == 0 && ch.num_segments > 0) {
            execute_chain(&ch);
        }
    }
}

/* Blocks/unblocks SIGCHLD around a foreground pipeline's own waitpid()
 * loop in executor.c. This closes a real race: without it, the async
 * sigchld_handler() below can reap a foreground child (via its own
 * WNOHANG waitpid loop) BEFORE execute_pipeline()'s explicit waitpid
 * gets a chance to see it -- when that happens, execute_pipeline()'s
 * waitpid immediately fails with ECHILD ("no child to wait for") and
 * silently falls back to its initialized exit status of 0, which
 * broke &&/||/; chaining for any foreground pipeline whose last stage
 * failed (e.g. `true | false && echo x` incorrectly ran the `echo`).
 * Blocking SIGCHLD guarantees the child's death is only ever observed
 * by execute_pipeline()'s own waitpid, never stolen by the handler.
 * This is the same technique real shells (e.g. bash's job control)
 * use for this exact hazard. */
void block_sigchld(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_BLOCK, &set, NULL);
}

void unblock_sigchld(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
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
 * foreground (we're at the prompt) and a `trap ... INT` is set, raise
 * the pending-trap flag for main.c to act on between commands, since
 * running the trap command directly here would not be async-signal-
 * safe. With no trap set and nothing in the foreground, do nothing,
 * matching how bash behaves at an empty prompt. */
static void sigint_handler(int signo) {
    if (foreground_pgid > 0) {
        kill(-foreground_pgid, signo);
        return;
    }
    if (int_trap_command != NULL) {
        int_trap_pending = 1;
    }
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
