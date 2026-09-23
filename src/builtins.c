#define _POSIX_C_SOURCE 200809L  /* kill() under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "acsh.h"

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

int try_run_builtin(Command *cmd, int *exit_status) {
    if (cmd->argc == 0 || cmd->argv[0] == NULL) {
        return 0;
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

    return 0;
}
