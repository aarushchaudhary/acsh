#define _POSIX_C_SOURCE 200809L  /* setpgid, WUNTRACED under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
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

    if (cmd->errfile != NULL) {
        int flags = O_WRONLY | O_CREAT | (cmd->err_append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->errfile, flags, 0644);
        if (fd < 0) {
            perror("acsh: open (errfile)");
            _exit(1);
        }
        dup2(fd, STDERR_FILENO);
        close(fd);
    }

    /* fd-duplication (e.g. "2>&1") is applied LAST, after any file
     * redirection above -- this ordering matters: `cmd > file 2>&1`
     * must make fd 2 point at the SAME PLACE fd 1 now points (the
     * file), not wherever fd 1 originally pointed. */
    if (cmd->dup_from >= 0 && cmd->dup_to >= 0) {
        if (dup2(cmd->dup_to, cmd->dup_from) < 0) {
            perror("acsh: dup2");
            _exit(1);
        }
    }
}

/* Child processes must reset the signal handling they inherited from
 * the shell: SIGINT/SIGTSTP go back to default (SIG_DFL) so Ctrl+C
 * actually kills the running program instead of being ignored/rerouted
 * like it is in the shell itself. SIGCHLD's blocked status must also
 * be cleared here: execute_pipeline() blocks SIGCHLD in the PARENT
 * from before this fork() happens (to avoid a real race explained at
 * that call site), and fork() copies the parent's blocked-signal mask
 * into the child -- so without this, every external program acsh
 * runs would inherit SIGCHLD blocked, which would break any of THEM
 * that forks its own children and expects to receive their SIGCHLD
 * normally (e.g. running another shell, or a build tool, inside acsh). */
static void reset_child_signals(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    unblock_sigchld();
}

/* Saved file descriptors from apply_redirection_with_restore(), so
 * redirect_restore() can put the parent process's stdin/stdout back
 * exactly as they were before a builtin/function (which runs IN the
 * parent process, not a forked child -- see the n==1 dispatch in
 * execute_pipeline() below) temporarily redirected them. -1 means
 * that fd was not touched and needs no restoring. */
typedef struct {
    int saved_stdin;
    int saved_stdout;
    int saved_stderr;
} RedirectSave;

/* Applies this command's redirection to the CURRENT process (the
 * shell's own parent process, for a builtin/function in the n==1
 * dispatch path -- it never forks). Unlike apply_redirection() (used
 * by actual forked children, which simply dup2() and then either
 * execvp() or _exit(), so the original fds never need restoring),
 * this version FIRST SAVES the process's real stdin/stdout via dup()
 * so redirect_restore() can put them back afterward. Without this,
 * a builtin's redirection would permanently redirect the
 * INTERACTIVE SHELL's own stdout for every command after it.
 *
 * This fixes a real bug found via test_acsh.sh: `type cd > file` (any
 * builtin/function with redirection, run standalone) silently did
 * nothing -- builtins in the n==1 parent-process path never called
 * ANY redirection logic at all before this fix existed. */
static RedirectSave apply_redirection_with_restore(Command *cmd) {
    RedirectSave save = { -1, -1, -1 };

    /* Flush BEFORE touching the fds: stdio buffers output internally,
     * so without this flush, text already written by this process
     * could still be sitting in the C library's buffer and end up
     * flushed to whatever fd 1 happens to point at AFTER this
     * function's dup2() calls run, rather than where it belonged. */
    fflush(stdout);

    if (cmd->infile != NULL) {
        int fd = open(cmd->infile, O_RDONLY);
        if (fd < 0) {
            perror("acsh: open (infile)");
            return save;
        }
        save.saved_stdin = dup(STDIN_FILENO);
        dup2(fd, STDIN_FILENO);
        close(fd);
    }

    if (cmd->outfile != NULL) {
        int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->outfile, flags, 0644);
        if (fd < 0) {
            perror("acsh: open (outfile)");
            return save;
        }
        save.saved_stdout = dup(STDOUT_FILENO);
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }

    if (cmd->errfile != NULL) {
        int flags = O_WRONLY | O_CREAT | (cmd->err_append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->errfile, flags, 0644);
        if (fd < 0) {
            perror("acsh: open (errfile)");
            return save;
        }
        save.saved_stderr = dup(STDERR_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }

    if (cmd->dup_from >= 0 && cmd->dup_to >= 0) {
        /* save whichever fd is about to be overwritten, if not
         * already saved above, so it gets restored afterward */
        if (cmd->dup_from == STDERR_FILENO && save.saved_stderr < 0) {
            save.saved_stderr = dup(STDERR_FILENO);
        } else if (cmd->dup_from == STDOUT_FILENO && save.saved_stdout < 0) {
            save.saved_stdout = dup(STDOUT_FILENO);
        }
        dup2(cmd->dup_to, cmd->dup_from);
    }

    return save;
}

/* Restores whatever apply_redirection_with_restore() saved. Flushes
 * stdout FIRST, before swapping the fd back, for the same buffering
 * reason noted above -- this is what actually gets the builtin's
 * output into the redirected destination before the original fd is
 * restored underneath it. */
static void redirect_restore(RedirectSave save) {
    fflush(stdout);
    if (save.saved_stdin >= 0) {
        dup2(save.saved_stdin, STDIN_FILENO);
        close(save.saved_stdin);
    }
    if (save.saved_stdout >= 0) {
        dup2(save.saved_stdout, STDOUT_FILENO);
        close(save.saved_stdout);
    }
    if (save.saved_stderr >= 0) {
        dup2(save.saved_stderr, STDERR_FILENO);
        close(save.saved_stderr);
    }
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

/* Returns 1 if `word` has the form NAME=value, where NAME is a valid
 * shell variable name (starts with a letter or underscore, then
 * letters/digits/underscores). This is how POSIX distinguishes an
 * assignment word from an ordinary command argument. */
static int is_assignment_word(const char *word) {
    if (!((word[0] >= 'A' && word[0] <= 'Z') ||
        (word[0] >= 'a' && word[0] <= 'z') || word[0] == '_')) {
        return 0;
        }
        size_t i = 1;
    while ((word[i] >= 'A' && word[i] <= 'Z') || (word[i] >= 'a' && word[i] <= 'z') ||
        (word[i] >= '0' && word[i] <= '9') || word[i] == '_') {
        i++;
        }
        return word[i] == '=';
}

/* Applies a NAME=value word by setting the variable in the process
 * environment (acsh's single variable store -- see the design note in
 * acsh.h next to env_expand). */
static void apply_assignment(const char *word) {
    const char *eq = strchr(word, '=');
    size_t name_len = (size_t)(eq - word);
    char name[256];
    if (name_len >= sizeof(name)) {
        fprintf(stderr, "acsh: variable name too long\n");
        return;
    }
    memcpy(name, word, name_len);
    name[name_len] = '\0';
    setenv(name, eq + 1, 1);
}

int execute_pipeline(Pipeline *pl) {
    int n = pl->num_cmds;

    /* Leading NAME=value words on a single command are assignments.
     * If they are the ONLY words (`x=5`), the line is just an
     * assignment and no command runs -- status is 0, or the status of
     * a command substitution in the value, which POSIX says a pure
     * assignment line reports (e.g. `x=$(false)` yields status 1).
     * If a command follows (`x=5 somecmd`), POSIX scopes the
     * assignment to that one command; acsh simplifies this to a
     * normal persistent assignment, noted as a known limitation. */
    if (n == 1) {
        Command *c = &pl->cmds[0];
        int skip = 0;
        while (skip < c->argc && is_assignment_word(c->argv[skip])) {
            apply_assignment(c->argv[skip]);
            skip++;
        }
        if (skip > 0) {
            if (skip == c->argc) {
                /* A pure assignment reports status 0 per POSIX -- unless
                 * a value contained a command substitution, in which
                 * case the status of THAT substitution (already
                 * recorded in $? when it ran during expansion) is what
                 * the line reports. Checking the raw text for a
                 * substitution marker tells the two cases apart;
                 * otherwise env_get_last_status() would leak in the
                 * PREVIOUS command's status (e.g. `false; x=5` wrongly
                 * reporting 1). */
                if (strstr(pl->raw_line, "$(") != NULL ||
                    strchr(pl->raw_line, '`') != NULL) {
                    return env_get_last_status();
                    }
                    return 0;
            }
            /* shift the remaining words down so the command is argv[0] */
            for (int i = skip; i <= c->argc; i++) {
                c->argv[i - skip] = c->argv[i];
            }
            c->argc -= skip;
        }
    }

    /* Single command, no pipe: check builtins first, then user-defined
     * functions. Both run in the parent process itself, not a forked
     * child -- for builtins this is required (e.g. `cd` must change
     * the shell's own directory); for functions it matters because a
     * function's `export`/variable assignments should affect the
     * calling shell's environment, exactly like a builtin's would,
     * not vanish when a child process exits. A function called as
     * part of a PIPELINE (n > 1) is a known scope limitation --
     * acsh does not support that; only standalone function calls do. */
    if (n == 1) {
        Command *only = &pl->cmds[0];
        int has_redirection = (only->infile != NULL || only->outfile != NULL ||
        only->errfile != NULL ||
        (only->dup_from >= 0 && only->dup_to >= 0));

        if (function_is_defined(only->argv[0]) && !has_redirection) {
            /* the common, simple case: a function with no
             * redirection -- skip the save/restore machinery entirely */
            return function_call(only->argc, only->argv);
        }

        RedirectSave save = { -1, -1, -1 };
        if (has_redirection) {
            save = apply_redirection_with_restore(only);
        }

        int exit_status = 0;
        int ran_builtin = try_run_builtin(only, &exit_status);
        int ran_function = 0;
        if (!ran_builtin && function_is_defined(only->argv[0])) {
            exit_status = function_call(only->argc, only->argv);
            ran_function = 1;
        }

        if (has_redirection) {
            redirect_restore(save);
        }

        if (ran_builtin || ran_function) {
            return exit_status;
        }
    }

    int prev_read_fd = -1;      /* read end of the previous pipe, or -1 */
    pid_t pids[ACSH_MAX_CMDS];
    pid_t pgid = 0;             /* process group ID for this whole pipeline */

    /* SIGCHLD is blocked from BEFORE the first fork() all the way
     * through the wait loop below (unblocked again once background-
     * or wait-handling is fully done for this pipeline). This closes
     * a real race that the previous version of this fix (blocking
     * only around the wait loop itself) still had: a fast-exiting
     * child -- e.g. `false`, which exits almost immediately -- can
     * deliver SIGCHLD and be reaped by the async sigchld_handler
     * BEFORE this function ever reaches its own waitpid() call,
     * making that waitpid() immediately fail with ECHILD and silently
     * report exit status 0 instead of the child's real status. This
     * was hard to catch because it's nondeterministic (depends on
     * exactly how fast the child exits relative to scheduling), and
     * it was hit specifically via command substitution (subst.c),
     * where each substitution runs its own pipeline in a fresh forked
     * subshell -- `$(false)` intermittently reported status 0. */
    block_sigchld();

    for (int i = 0; i < n; i++) {
        Command *cmd = &pl->cmds[i];
        int pipefd[2] = { -1, -1 };
        int have_next = (i < n - 1);

        if (have_next) {
            if (pipe(pipefd) < 0) {
                perror("acsh: pipe");
                unblock_sigchld();
                return 1;
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("acsh: fork");
            unblock_sigchld();
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

            /* A builtin or function used as ONE STAGE of a multi-
             * command pipeline (e.g. `alias | grep ll`, or a function
             * piped into something) was previously never recognized
             * here at all -- try_run_builtin()/function_call() were
             * only ever consulted for the n==1 (single command, no
             * pipe) case earlier in this function, so a builtin name
             * anywhere inside a pipe fell straight through to
             * execvp() and failed with "command not found". Since
             * each pipeline stage is already its own forked child
             * process, running the builtin/function HERE and exiting
             * with its status is both correct and safe. Found via
             * test_acsh.sh (`alias | grep ll` failed outright). */
            {
                int exit_status = 0;
                if (try_run_builtin(cmd, &exit_status)) {
                    fflush(stdout);
                    _exit(exit_status);
                }
                if (function_is_defined(cmd->argv[0])) {
                    exit_status = function_call(cmd->argc, cmd->argv);
                    fflush(stdout);
                    _exit(exit_status);
                }
            }

            execvp(cmd->argv[0], cmd->argv);
            /* execvp only returns on failure -- errno tells us WHY,
             * and giving the real reason (rather than one generic
             * "command not found" for every case) is both more
             * accurate and more actionable: a permissions problem and
             * a genuine typo need different fixes. */
            if (errno == ENOENT) {
                fprintf(stderr, "acsh: %s: command not found\n", cmd->argv[0]);
            } else if (errno == EACCES) {
                fprintf(stderr, "acsh: %s: permission denied (found, but not executable -- "
                "check file permissions or a missing #!/.../interpreter line)\n",
                cmd->argv[0]);
            } else if (errno == ENOEXEC) {
                fprintf(stderr, "acsh: %s: cannot execute (not a valid executable -- "
                "if this is a script, it may be missing its #!/bin/sh line)\n",
                cmd->argv[0]);
            } else {
                fprintf(stderr, "acsh: %s: cannot execute: %s\n", cmd->argv[0], strerror(errno));
            }
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
        /* unblock before returning: a backgrounded pipeline's own
         * SIGCHLD (whenever it finishes) must reach the async handler
         * normally, since nothing else will ever wait() for it */
        unblock_sigchld();
        /* A backgrounded pipeline has no exit status to report yet,
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
     * SIGCHLD has already been blocked since before this pipeline's
     * first fork() (see the comment above that block_sigchld() call),
     * so no child of this pipeline can be reaped out from under the
     * explicit waitpid() loop below. */
    set_foreground_pgid(pgid);

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
        pipeline_free(&pl);
    }
}
