#define _POSIX_C_SOURCE 200809L  /* kill() under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "acsh.h"

/* Single source of truth for "is NAME a builtin", used by the `type`
 * and `command -v` builtins below. Kept as an explicit list (rather
 * than, say, deriving it from try_run_builtin()'s dispatch chain
 * automatically) because C has no clean way to introspect that chain
 * -- this list must be kept in sync with it by hand. It mirrors
 * try_run_builtin()'s strcmp() chain exactly as of this writing. */
static const char *BUILTIN_NAMES[] = {
    "break", "continue", "test", "[", "[[", "cd", "exit", "pwd", "jobs", "fg", "bg",
    "export", "unset", "alias", "unalias", "history", "source", ".",
    "read", "exec", "umask", "type", "command", "shift", "local", "set", "eval", "trap",
    NULL
};

static int is_builtin_name(const char *name) {
    for (int i = 0; BUILTIN_NAMES[i] != NULL; i++) {
        if (strcmp(BUILTIN_NAMES[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Searches $PATH for `name`, the same way execvp() would, and returns
 * a newly malloc'd full path if found, or NULL if not. Used by `type`
 * and `command -v` to report where an external command would be
 * found, without actually running it. */
static char *find_in_path(const char *name) {
    if (strchr(name, '/') != NULL) {
        /* already a path (relative or absolute) -- check it directly */
        if (access(name, X_OK) == 0) {
            return strdup(name);
        }
        return NULL;
    }

    const char *path_env = getenv("PATH");
    if (path_env == NULL) {
        return NULL;
    }

    char *path_copy = strdup(path_env);
    if (path_copy == NULL) {
        return NULL;
    }

    char *result = NULL;
    char *dir = strtok(path_copy, ":");
    while (dir != NULL) {
        char candidate[ACSH_MAX_LINE];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, name);
        if (access(candidate, X_OK) == 0) {
            result = strdup(candidate);
            break;
        }
        dir = strtok(NULL, ":");
    }

    free(path_copy);
    return result;
}

static int builtin_cd(Command *cmd) {
    const char *target = (cmd->argc > 1) ? cmd->argv[1] : getenv("HOME");
    if (target == NULL) {
        fprintf(stderr, "acsh: cd: HOME not set\n");
        return 1;
    }
    if (chdir(target) != 0) {
        perror("acsh: cd");
        return 1;
    }
    return 0;
}

static int builtin_exit(Command *cmd) {
    int code = 0;
    if (cmd->argc > 1) {
        code = atoi(cmd->argv[1]);
    }
    /* Run any `trap ... EXIT` before actually terminating -- exit()
     * jumps straight out of the process and never returns to
     * main()'s own loop, so main() calling trap_run_exit() after its
     * loop ends only covers the EOF/Ctrl+D path, not this one. Both
     * paths out of the shell must honor an EXIT trap. */
    trap_run_exit();
    exit(code);
    return 0; /* unreachable */
}

static int builtin_pwd(void) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("%s\n", cwd);
        return 0;
    }
    perror("acsh: pwd");
    return 1;
}

static int builtin_jobs(void) {
    jobs_print_all();
    jobs_cleanup_done();
    return 0;
}

/* Parses a "%N" or plain "N" job-id argument. If no argument is given,
 * falls back to the most recently added job (matches bash behaviour
 * for bare `fg`/`bg`). Returns NULL if nothing matches. */
static Job *resolve_job_arg(Command *cmd) {
    if (cmd->argc < 2) {
        return jobs_find_most_recent();
    }
    const char *arg = cmd->argv[1];
    if (arg[0] == '%') {
        arg++;
    }
    return jobs_find_by_id(atoi(arg));
}

static int builtin_fg(Command *cmd) {
    Job *j = resolve_job_arg(cmd);
    if (j == NULL) {
        fprintf(stderr, "acsh: fg: no such job\n");
        return 1;
    }

    printf("%s\n", j->cmdline);

    /* resume it if it was stopped, then wait for it like any
     * foreground pipeline */
    kill(-j->pgid, SIGCONT);
    jobs_mark_running(j->pgid);

    set_foreground_pgid(j->pgid);
    int status;
    int remaining = j->num_pids;
    while (remaining > 0) {
        pid_t w = waitpid(-j->pgid, &status, WUNTRACED);
        if (w < 0) break;
        if (WIFSTOPPED(status)) {
            jobs_mark_stopped(j->pgid);
            break;
        }
        remaining--;
    }
    set_foreground_pgid(0);
    return 0;
}

static int builtin_bg(Command *cmd) {
    Job *j = resolve_job_arg(cmd);
    if (j == NULL) {
        fprintf(stderr, "acsh: bg: no such job\n");
        return 1;
    }
    kill(-j->pgid, SIGCONT);
    jobs_mark_running(j->pgid);
    printf("[%d] %s &\n", j->id, j->cmdline);
    return 0;
}

/* `export NAME=value` sets a real process environment variable, so
 * $NAME becomes visible both to acsh's own $VAR expansion (env.c) and
 * to any program acsh execs afterwards -- setenv() affects the
 * process's environ[], which is inherited by every child via fork().
 * `export NAME` (no =value) with no existing value exports it as
 * empty, matching common shell behaviour for that form. Multiple
 * NAME=value pairs on one line are all applied. */
static int builtin_export(Command *cmd) {
    if (cmd->argc < 2) {
        /* bare `export` with no args: POSIX says list all exported
         * vars; skipped for this mini-project, just a no-op */
        return 0;
    }

    int status = 0;
    for (int i = 1; i < cmd->argc; i++) {
        char *arg = cmd->argv[i];
        char *eq = strchr(arg, '=');

        if (eq != NULL) {
            size_t name_len = (size_t)(eq - arg);
            char name[256];
            if (name_len >= sizeof(name)) {
                fprintf(stderr, "acsh: export: variable name too long\n");
                status = 1;
                continue;
            }
            memcpy(name, arg, name_len);
            name[name_len] = '\0';

            if (setenv(name, eq + 1, 1) != 0) {
                perror("acsh: export");
                status = 1;
            }
        } else {
            /* bare NAME: export with current value if set, else empty */
            const char *existing = getenv(arg);
            if (setenv(arg, existing != NULL ? existing : "", 1) != 0) {
                perror("acsh: export");
                status = 1;
            }
        }
    }
    return status;
}

static int builtin_unset(Command *cmd) {
    if (cmd->argc < 2) {
        fprintf(stderr, "acsh: unset: usage: unset NAME [NAME ...]\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < cmd->argc; i++) {
        if (unsetenv(cmd->argv[i]) != 0) {
            perror("acsh: unset");
            status = 1;
        }
    }
    return status;
}

static int builtin_alias(Command *cmd) {
    if (cmd->argc < 2) {
        alias_print_all();
        return 0;
    }
    for (int i = 1; i < cmd->argc; i++) {
        char *arg = cmd->argv[i];
        char *eq = strchr(arg, '=');
        if (eq == NULL) {
            /* `alias name` with no = : show just that one alias */
            const char *val = alias_get(arg);
            if (val != NULL) {
                printf("alias %s='%s'\n", arg, val);
            } else {
                fprintf(stderr, "acsh: alias: %s: not found\n", arg);
            }
            continue;
        }
        size_t name_len = (size_t)(eq - arg);
        char name[64];
        if (name_len >= sizeof(name)) {
            fprintf(stderr, "acsh: alias: name too long\n");
            continue;
        }
        memcpy(name, arg, name_len);
        name[name_len] = '\0';
        alias_set(name, eq + 1);
    }
    return 0;
}

static int builtin_unalias(Command *cmd) {
    if (cmd->argc < 2) {
        fprintf(stderr, "acsh: unalias: usage: unalias NAME [NAME ...]\n");
        return 1;
    }
    for (int i = 1; i < cmd->argc; i++) {
        alias_unset(cmd->argv[i]);
    }
    return 0;
}

static int builtin_history(void) {
    history_print_all();
    return 0;
}

/* `read VAR1 VAR2 ... VARN` reads one line from stdin, splits it on
 * whitespace, and assigns the first N-1 words to VAR1..VAR(N-1) with
 * the REMAINDER of the line (not just the Nth word) going to the
 * last variable -- this is standard POSIX read behaviour, e.g.
 * `read first rest` on "one two three" gives first=one,
 * rest="two three". `read` with no variable names reads into the
 * variable REPLY, matching bash/ash convention. Returns 1 (not 0) on
 * EOF with nothing read, so `while read line; do ...; done < file`
 * terminates correctly when the input is exhausted. */
/* `exec COMMAND ARGS...` replaces the CURRENT process (the shell
 * itself) with COMMAND, via execvp() -- it never returns if COMMAND
 * is found, since the shell's own process image is gone at that
 * point. This matters for scripts/interactive use where the point is
 * to hand off control entirely (e.g. `exec some_program` as the last
 * line of a wrapper script) rather than spawn a child and wait for
 * it. `exec` with NO arguments is a no-op in real shells (historically
 * used just to apply redirections permanently to the current shell);
 * acsh treats a bare `exec` as a no-op returning success, which is a
 * reasonable simplification since acsh has no persistent-redirection
 * state separate from a command to apply it to. */
/* `umask` with no args prints the current file-creation mask in
 * octal (e.g. "0022"); `umask MODE` sets it. Uses the real umask()
 * syscall, so this genuinely affects permissions of files created by
 * anything run afterwards -- not just a cosmetic setting. */
static int builtin_umask(Command *cmd) {
    if (cmd->argc <= 1) {
        /* umask() itself both sets AND returns the old value, so to
         * just READ the current mask without changing it, set it to
         * some value and immediately set it back to what that call
         * reported as the previous mask */
        mode_t current = umask(0);
        umask(current);
        printf("%04o\n", current);
        return 0;
    }

    char *endptr;
    long mode = strtol(cmd->argv[1], &endptr, 8);
    if (*endptr != '\0' || mode < 0 || mode > 0777) {
        fprintf(stderr, "acsh: umask: %s: invalid octal mode\n", cmd->argv[1]);
        return 1;
    }
    umask((mode_t)mode);
    return 0;
}

/* `type NAME...` reports, for each NAME, whether it's an alias, a
 * shell function, a builtin, or found on $PATH (in that priority
 * order, matching how acsh itself would actually resolve and run
 * it), or reports "not found". Does not run anything -- purely
 * informational, which is the whole point of `type` as distinct from
 * just trying to run NAME and seeing what happens. */
static int builtin_type(Command *cmd) {
    if (cmd->argc <= 1) {
        fprintf(stderr, "acsh: type: usage: type NAME [NAME ...]\n");
        return 1;
    }

    int any_not_found = 0;
    for (int i = 1; i < cmd->argc; i++) {
        const char *name = cmd->argv[i];

        const char *alias_val = alias_get(name);
        if (alias_val != NULL) {
            printf("%s is aliased to '%s'\n", name, alias_val);
            continue;
        }
        if (function_is_defined(name)) {
            printf("%s is a shell function\n", name);
            continue;
        }
        if (is_builtin_name(name)) {
            printf("%s is a shell builtin\n", name);
            continue;
        }
        char *path = find_in_path(name);
        if (path != NULL) {
            printf("%s is %s\n", name, path);
            free(path);
            continue;
        }
        fprintf(stderr, "acsh: type: %s: not found\n", name);
        any_not_found = 1;
    }

    return any_not_found ? 1 : 0;
}

/* `command NAME ARGS...` runs NAME as an ORDINARY COMMAND, bypassing
 * any shell function or alias of the same name -- useful when a
 * script has redefined e.g. `ls` as a function but still wants the
 * real `ls` binary. `command -v NAME` instead just PRINTS where NAME
 * would resolve to (like a quieter `type`), without running it, and
 * is commonly used in portable scripts to check whether something
 * exists before using it. acsh implements both forms. */
static int builtin_command(Command *cmd) {
    if (cmd->argc <= 1) {
        fprintf(stderr, "acsh: command: usage: command [-v] NAME [ARGS ...]\n");
        return 1;
    }

    if (strcmp(cmd->argv[1], "-v") == 0) {
        if (cmd->argc <= 2) {
            fprintf(stderr, "acsh: command: -v: usage: command -v NAME\n");
            return 1;
        }
        const char *name = cmd->argv[2];
        if (is_builtin_name(name)) {
            printf("%s\n", name);
            return 0;
        }
        char *path = find_in_path(name);
        if (path != NULL) {
            printf("%s\n", path);
            free(path);
            return 0;
        }
        return 1; /* not found: command -v is silent on failure by convention */
    }

    /* plain `command NAME ARGS...`: build a Command skipping
     * functions/aliases (bypass those, go straight to builtin-or-
     * external resolution) by constructing argv shifted left by one
     * and running it through the normal single-command path, EXCEPT
     * we deliberately do NOT check function_is_defined() here -- that
     * is the entire point of `command`. */
    Command shifted;
    shifted.argc = cmd->argc - 1;
    for (int i = 0; i < shifted.argc; i++) {
        shifted.argv[i] = cmd->argv[i + 1];
    }
    shifted.argv[shifted.argc] = NULL;
    shifted.infile = cmd->infile;
    shifted.outfile = cmd->outfile;
    shifted.append = cmd->append;

    int exit_status = 0;
    if (try_run_builtin(&shifted, &exit_status)) {
        return exit_status;
    }

    /* not a builtin: run it as an external command directly (NOT
     * through execute_pipeline(), to avoid re-entering the
     * function-dispatch path this builtin exists to skip) */
    pid_t pid = fork();
    if (pid < 0) {
        perror("acsh: command: fork");
        return 1;
    }
    if (pid == 0) {
        execvp(shifted.argv[0], shifted.argv);
        fprintf(stderr, "acsh: command: %s: command not found\n", shifted.argv[0]);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int builtin_exec(Command *cmd) {
    if (cmd->argc <= 1) {
        return 0; /* bare `exec`: no-op, see note above */
    }

    /* argv[1..] becomes the new process's argv[0..] */
    execvp(cmd->argv[1], cmd->argv + 1);

    /* only reached if execvp failed */
    if (errno == ENOENT) {
        fprintf(stderr, "acsh: exec: %s: command not found\n", cmd->argv[1]);
    } else {
        fprintf(stderr, "acsh: exec: %s: %s\n", cmd->argv[1], strerror(errno));
    }
    /* POSIX: if exec's target can't be found/run, the shell that
     * invoked `exec` (if non-interactive, i.e. a script) must exit;
     * acsh simplifies this to always exiting on exec failure, since
     * distinguishing "interactive" vs "script" mode isn't otherwise
     * tracked anywhere in the shell */
    trap_run_exit();
    exit(127);
}

/* `eval ARG1 ARG2 ...` joins all its arguments with spaces into one
 * string, then parses and runs THAT as a brand new command line --
 * this is what lets a variable holding shell syntax (e.g. a
 * dynamically-built command) actually be executed rather than just
 * treated as a literal argument. Runs through the full chain
 * machinery (parse_chain/execute_chain), so eval'd text can itself
 * contain pipes, &&/||/;, and even control-flow keywords. */
/* `shift [N]` discards the first N positional parameters of the
 * CURRENT function call (default N=1), so $1 becomes what was
 * $((1+N)), and so on -- the standard way to loop over "$@" one
 * argument at a time in a POSIX function: `while test $# -gt 0; do
 * echo $1; shift; done`. */
/* `local NAME` or `local NAME=value` (one or more, space-separated)
 * declares NAME as scoped to the CURRENTLY EXECUTING FUNCTION CALL --
 * its value (or absence) before the call is restored automatically
 * when the function returns. Only meaningful inside a function body;
 * acsh follows real shells in treating `local` outside any function
 * as an error rather than silently doing something else. */
/* `set` is a large, multi-purpose POSIX builtin; acsh implements only
 * its most commonly used form in practice: `set -- ARG1 ARG2 ...`,
 * which REPLACES the current function call's positional parameters
 * ($1, $2, ...) with the given arguments -- useful for e.g.
 * re-splitting a variable into $1.../$@ inside a function. Other `set`
 * forms (`set -e`, `set -x`, etc. to toggle shell options like
 * exit-on-error or command tracing) are ACCEPTED without error (so a
 * script using them doesn't fail outright) but have NO EFFECT --
 * acsh does not implement those behaviours. This is a deliberate,
 * documented scope limitation: implementing errexit/xtrace properly
 * touches nearly every execution path in the shell, which was judged
 * not to fit this project's scope once control flow, functions, and
 * command substitution were already the priority. */
/* `trap COMMAND SIGNAME` registers COMMAND to run when SIGNAME
 * occurs; `trap - SIGNAME` (or `trap '' SIGNAME`) resets it to no
 * trap. acsh supports exactly two signal names: INT (Ctrl+C at the
 * prompt) and EXIT (run once when the shell exits) -- see the scope
 * note in acsh.h above the trap_* declarations for why the full
 * POSIX signal set isn't implemented. */
static int builtin_trap(Command *cmd) {
    if (cmd->argc < 3) {
        fprintf(stderr, "acsh: trap: usage: trap COMMAND SIGNAME   (SIGNAME: INT or EXIT)\n"
        "  acsh: trap - SIGNAME   to clear a trap\n");
        return 1;
    }

    const char *command_arg = cmd->argv[1];
    const char *signame = cmd->argv[2];
    int clearing = (strcmp(command_arg, "-") == 0 || command_arg[0] == '\0');
    const char *command = clearing ? NULL : command_arg;

    if (strcmp(signame, "INT") == 0) {
        trap_set_int(command);
        return 0;
    }
    if (strcmp(signame, "EXIT") == 0) {
        trap_set_exit(command);
        return 0;
    }

    fprintf(stderr, "acsh: trap: %s: unsupported signal name (acsh supports "
    "only INT and EXIT)\n", signame);
    return 1;
}

static int builtin_set(Command *cmd) {
    if (cmd->argc >= 2 && strcmp(cmd->argv[1], "--") == 0) {
        int new_argc = cmd->argc - 2;
        char **new_argv = cmd->argv + 2;
        if (env_set_positional_params(new_argc, new_argv) != 0) {
            fprintf(stderr, "acsh: set --: not inside a function (acsh has no "
            "top-level positional parameters to set)\n");
            return 1;
        }
        return 0;
    }

    /* any other `set` invocation (set -e, set -x, bare `set`, etc.):
     * accept silently, no effect -- see the scope-limitation note
     * above this function */
    return 0;
}

static int builtin_local(Command *cmd) {
    if (cmd->argc <= 1) {
        fprintf(stderr, "acsh: local: usage: local NAME[=value] [NAME[=value] ...]\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < cmd->argc; i++) {
        char *arg = cmd->argv[i];
        char *eq = strchr(arg, '=');

        if (eq != NULL) {
            size_t name_len = (size_t)(eq - arg);
            char name[256];
            if (name_len >= sizeof(name)) {
                fprintf(stderr, "acsh: local: variable name too long\n");
                status = 1;
                continue;
            }
            memcpy(name, arg, name_len);
            name[name_len] = '\0';
            if (env_declare_local(name, eq + 1) != 0) {
                fprintf(stderr, "acsh: local: %s: not inside a function\n", name);
                status = 1;
            }
        } else {
            if (env_declare_local(arg, NULL) != 0) {
                fprintf(stderr, "acsh: local: %s: not inside a function\n", arg);
                status = 1;
            }
        }
    }
    return status;
}

static int builtin_shift(Command *cmd) {
    int n = 1;
    if (cmd->argc > 1) {
        n = atoi(cmd->argv[1]);
    }
    if (env_shift_positional_params(n) != 0) {
        fprintf(stderr, "acsh: shift: cannot shift %d (not enough positional "
        "parameters, or not inside a function)\n", n);
        return 1;
    }
    return 0;
}

static int builtin_eval(Command *cmd) {
    if (cmd->argc <= 1) {
        return 0; /* `eval` with nothing to evaluate is a no-op */
    }

    size_t total_len = 0;
    for (int i = 1; i < cmd->argc; i++) {
        total_len += strlen(cmd->argv[i]) + 1;
    }
    char *joined = malloc(total_len + 1);
    if (joined == NULL) {
        return 1;
    }
    joined[0] = '\0';
    for (int i = 1; i < cmd->argc; i++) {
        strcat(joined, cmd->argv[i]);
        if (i < cmd->argc - 1) strcat(joined, " ");
    }

    Chain ch;
    int status = 0;
    if (parse_chain(joined, &ch) == 0 && ch.num_segments > 0) {
        execute_chain(&ch);
        status = env_get_last_status();
    }

    free(joined);
    return status;
}

static int builtin_read(Command *cmd) {
    char line[ACSH_MAX_LINE];
    if (fgets(line, sizeof(line), stdin) == NULL) {
        return 1; /* EOF */
    }
    line[strcspn(line, "\n")] = '\0';

    int nvars = cmd->argc - 1;
    const char *default_var = "REPLY";
    if (nvars == 0) {
        /* no variable names given: whole line goes into $REPLY */
        setenv(default_var, line, 1);
        return 0;
    }

    char *p = line;
    for (int i = 0; i < nvars; i++) {
        while (*p == ' ' || *p == '\t') p++;

        if (i == nvars - 1) {
            /* last variable gets everything remaining, including any
             * further whitespace-separated words -- not just one word */
            setenv(cmd->argv[i + 1], p, 1);
            break;
        }

        char *word_start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') p++;
        char saved = *p;
        *p = '\0';
        setenv(cmd->argv[i + 1], word_start, 1);
        *p = saved;
        if (saved != '\0') p++;
    }

    return 0;
}

static int builtin_source(Command *cmd) {
    if (cmd->argc < 2) {
        fprintf(stderr, "acsh: source: usage: source FILE\n");
        return 1;
    }
    return (run_script_file(cmd->argv[1]) == 0) ? 0 : 1;
}

static int builtin_break_or_continue(Command *cmd, int is_continue) {
    if (control_flow_in_loop_depth() <= 0) {
        /* Matches real shells: not inside any loop is a warning, not
         * a hard error -- the shell keeps running normally. */
        fprintf(stderr, "acsh: %s: only meaningful inside a loop\n",
                is_continue ? "continue" : "break");
        return 0;
    }

    int level = 1;
    if (cmd->argc > 1) {
        level = atoi(cmd->argv[1]);
        if (level < 1) level = 1;
    }

    control_flow_signal_loop(is_continue, level);
    return 0;
}

int try_run_builtin(Command *cmd, int *exit_status) {
    if (cmd->argc == 0 || cmd->argv[0] == NULL) {
        return 0;
    }

    if (strcmp(cmd->argv[0], "break") == 0) {
        *exit_status = builtin_break_or_continue(cmd, /*is_continue=*/0);
        return 1;
    }
    if (strcmp(cmd->argv[0], "continue") == 0) {
        *exit_status = builtin_break_or_continue(cmd, /*is_continue=*/1);
        return 1;
    }

    if (strcmp(cmd->argv[0], "test") == 0 || strcmp(cmd->argv[0], "[") == 0 ||
        strcmp(cmd->argv[0], "[[") == 0) {
        *exit_status = builtin_test(cmd);
    return 1;
        }

        if (strcmp(cmd->argv[0], "cd") == 0) {
            *exit_status = builtin_cd(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "exit") == 0) {
            *exit_status = builtin_exit(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "pwd") == 0) {
            *exit_status = builtin_pwd();
            return 1;
        }
        if (strcmp(cmd->argv[0], "jobs") == 0) {
            *exit_status = builtin_jobs();
            return 1;
        }
        if (strcmp(cmd->argv[0], "fg") == 0) {
            *exit_status = builtin_fg(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "bg") == 0) {
            *exit_status = builtin_bg(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "export") == 0) {
            *exit_status = builtin_export(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "unset") == 0) {
            *exit_status = builtin_unset(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "alias") == 0) {
            *exit_status = builtin_alias(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "unalias") == 0) {
            *exit_status = builtin_unalias(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "history") == 0) {
            *exit_status = builtin_history();
            return 1;
        }
        if (strcmp(cmd->argv[0], "source") == 0 || strcmp(cmd->argv[0], ".") == 0) {
            *exit_status = builtin_source(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "read") == 0) {
            *exit_status = builtin_read(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "exec") == 0) {
            *exit_status = builtin_exec(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "umask") == 0) {
            *exit_status = builtin_umask(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "type") == 0) {
            *exit_status = builtin_type(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "command") == 0) {
            *exit_status = builtin_command(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "shift") == 0) {
            *exit_status = builtin_shift(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "local") == 0) {
            *exit_status = builtin_local(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "set") == 0) {
            *exit_status = builtin_set(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "eval") == 0) {
            *exit_status = builtin_eval(cmd);
            return 1;
        }
        if (strcmp(cmd->argv[0], "trap") == 0) {
            *exit_status = builtin_trap(cmd);
            return 1;
        }

        return 0;
}
