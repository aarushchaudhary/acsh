#define _POSIX_C_SOURCE 200809L  /* setpgid, WUNTRACED under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include "acsh.h"

/* Applies this command's redirection (< , > , >>) to the CURRENT
 * process using dup2(). Must be called after fork(), inside the child,
 * before execvp(). */
static void apply_redirection(Command *cmd) {
    if (cmd->infile != NULL) {
        int fd = open(cmd->infile, O_RDONLY);
        if (fd < 0) {
            perror("acsh: open (infile)");
            _exit(1);
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }

    if (cmd->outfile != NULL) {
        int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->outfile, flags, 0644);
        if (fd < 0) {
            perror("acsh: open (outfile)");
            _exit(1);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
}

/* Child processes must reset the signal handling they inherited from
 * the shell: SIGINT/SIGTSTP go back to default (SIG_DFL) so Ctrl+C
 * actually kills the running program instead of being ignored/rerouted
 * like it is in the shell itself. */
static void reset_child_signals(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

void execute_pipeline(Pipeline *pl) {
    int n = pl->num_cmds;

    /* Single command, no pipe: check builtins first. Builtins must run
     * in the parent process itself (e.g. `cd` has to change the
     * shell's own working directory, not a child's). */
    if (n == 1) {
        int exit_status = 0;
        if (try_run_builtin(&pl->cmds[0], &exit_status)) {
            return;
        }
    }

    int prev_read_fd = -1;      /* read end of the previous pipe, or -1 */
    pid_t pids[ACSH_MAX_CMDS];
    pid_t pgid = 0;             /* process group ID for this whole pipeline */

    for (int i = 0; i < n; i++) {
        Command *cmd = &pl->cmds[i];
        int pipefd[2] = { -1, -1 };
        int have_next = (i < n - 1);

        if (have_next) {
            if (pipe(pipefd) < 0) {
                perror("acsh: pipe");
                return;
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("acsh: fork");
            return;
        }

        if (pid == 0) {
            /* ---- child ---- */
            reset_child_signals();

            /* Put every process of this pipeline into ONE process
             * group, named after the first command's pid. This is
             * what lets us send SIGINT/SIGTSTP to the whole pipeline
             * at once with kill(-pgid, sig), and is exactly how real
             * shells implement job control. */
            pid_t my_pgid = (pgid == 0) ? getpid() : pgid;
            setpgid(0, my_pgid);

            if (prev_read_fd != -1) {
                dup2(prev_read_fd, STDIN_FILENO);
                close(prev_read_fd);
            }
            if (have_next) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }

            apply_redirection(cmd);

            execvp(cmd->argv[0], cmd->argv);
            fprintf(stderr, "acsh: %s: command not found\n", cmd->argv[0]);
            _exit(127);
        }

        /* ---- parent ---- */
        if (pgid == 0) {
            pgid = pid;   /* first child's pid becomes the group's pgid */
        }
        /* Parent ALSO calls setpgid on the child (redundant with the
         * child's own call above, but avoids a race: whichever of the
         * two runs first "wins", and doing it in both places means we
         * don't depend on scheduling order). */
        setpgid(pid, pgid);

        pids[i] = pid;

        if (prev_read_fd != -1) {
            close(prev_read_fd);
        }
        if (have_next) {
            close(pipefd[1]);
            prev_read_fd = pipefd[0];
        }
    }

    if (pl->background) {
        int job_id = jobs_add(pgid, pids, n, pl->raw_line);
        printf("[%d] %d\n", job_id, pgid);
        /* do NOT wait -- shell returns to the prompt immediately;
         * SIGCHLD handler in signals.c reaps it whenever it finishes */
    } else {
        /* foreground: tell the signal handlers who Ctrl+C/Ctrl+Z
         * should go to, then block until the whole pipeline is done
         * or stopped. */
        set_foreground_pgid(pgid);

        int status;
        int remaining = n;
        while (remaining > 0) {
            pid_t w = waitpid(-pgid, &status, WUNTRACED);
            if (w < 0) {
                break; /* ECHILD: nothing left to wait for */
            }
            if (WIFSTOPPED(status)) {
                /* user hit Ctrl+Z: register as a stopped job and give
                 * the prompt back, like a real shell would */
                jobs_add(pgid, pids, n, pl->raw_line);
                jobs_mark_stopped(pgid);
                break;
            }
            remaining--;
        }

        set_foreground_pgid(0); /* back to "nothing in foreground" */
    }
}
