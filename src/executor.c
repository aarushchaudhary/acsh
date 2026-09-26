#define _POSIX_C_SOURCE 200809L  /* setpgid, WUNTRACED under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Converts a raw wait() status into a shell-style exit code: normal
 * exit -> the exit() value (0-255); killed by signal -> 128+signum,
 * which is the same convention bash/ash use so `$?` after a
 * Ctrl+C-killed command reads the way people expect. */
static int status_to_exit_code(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 0;
}

int execute_pipeline(Pipeline *pl) {
    int n = pl->num_cmds;

    /* Single command, no pipe: check builtins first. Builtins must run
     * in the parent process itself (e.g. `cd` has to change the
     * shell's own working directory, not a child's). */
    if (n == 1) {
        int exit_status = 0;
        if (try_run_builtin(&pl->cmds[0], &exit_status)) {
            return exit_status;
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
                return 1;
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("acsh: fork");
            return 1;
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

    pid_t last_pid = pids[n - 1];

    if (pl->background) {
        int job_id = jobs_add(pgid, pids, n, pl->raw_line);
        printf("[%d] %d\n", job_id, pgid);
        /* do NOT wait -- shell returns to the prompt immediately;
         * SIGCHLD handler in signals.c reaps it whenever it finishes.
         * A backgrounded pipeline has no exit status to report yet,
         * so &&/||/; after it always treat it as success (POSIX's
         * behaviour for `cmd &` is exactly this: $? is 0 right away). */
        return 0;
    }

    /* foreground: tell the signal handlers who Ctrl+C/Ctrl+Z should go
     * to, then block until the whole pipeline is done or stopped. We
     * specifically capture the LAST command's exit status, since a
     * pipeline's overall status in POSIX is the last stage's status
     * (e.g. `false | true` "succeeds" even though `false` failed).
     *
     * SIGCHLD is blocked for the duration of this wait loop to avoid
     * a race with the async sigchld_handler (see block_sigchld()'s
     * comment in signals.c for the full explanation). */
    set_foreground_pgid(pgid);
    block_sigchld();

    int status = 0;
    int last_exit_code = 0;
    int remaining = n;
    while (remaining > 0) {
        pid_t w = waitpid(-pgid, &status, WUNTRACED);
        if (w < 0) {
            break; /* ECHILD: nothing left to wait for */
        }
        if (WIFSTOPPED(status)) {
            /* user hit Ctrl+Z: register as a stopped job and give the
             * prompt back, like a real shell would. Exit status is
             * left at whatever it was (POSIX behaviour here varies by
             * shell; treating it as "success so far" is reasonable
             * for this mini-project's scope). */
            jobs_add(pgid, pids, n, pl->raw_line);
            jobs_mark_stopped(pgid);
            break;
        }
        if (w == last_pid) {
            last_exit_code = status_to_exit_code(status);
        }
        remaining--;
    }

    unblock_sigchld();
    set_foreground_pgid(0); /* back to "nothing in foreground" */
    return last_exit_code;
}

void execute_chain(Chain *ch) {
    int status = 0;

    for (int i = 0; i < ch->num_segments; i++) {
        if (i > 0) {
            ChainOp prev_op = ch->ops[i - 1];
            if (prev_op == CHAIN_AND && status != 0) {
                continue; /* previous failed, skip this && stage */
            }
            if (prev_op == CHAIN_OR && status == 0) {
                continue; /* previous succeeded, skip this || stage */
            }
            /* CHAIN_SEQ (;) always runs the next stage regardless */
        }

        /* Alias expansion happens on this stage's raw text, exactly
         * now -- not upfront -- so `cmd1 ; alias_cmd` can rely on
         * alias state cmd1 itself might have changed (e.g. cmd1 is
         * literally `alias foo=bar`). */
        char *raw = ch->segments[i];
        char *aliased = alias_expand_line(raw);
        char *text_to_parse = (aliased != NULL) ? aliased : raw;

        /* parse_line() calls env_expand() on every word as it builds
         * the Pipeline -- THIS is where $VAR / $? / ~ get resolved,
         * and it happens right here, per stage, so $? reflects the
         * PREVIOUS STAGE IN THIS SAME CHAIN, not a stale value from
         * before the chain started. */
        Pipeline pl;
        memset(&pl, 0, sizeof(pl));
        int rc = parse_line(text_to_parse, &pl);
        free(aliased);

        if (rc != 0) {
            /* parse_line already printed the error; stop the chain
             * here, matching how a real shell aborts on a syntax
             * error rather than continuing past broken syntax */
            return;
        }
        if (pl.num_cmds == 0) {
            continue;
        }

        status = execute_pipeline(&pl);
        env_set_last_status(status);
    }
}
