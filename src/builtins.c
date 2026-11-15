#define _DEFAULT_SOURCE           /* Linux resource limits (RLIMIT_NICE, ...) */
#define _POSIX_C_SOURCE 200809L  /* kill(), strsignal() under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <poll.h>
#include <termios.h>
#include <ctype.h>
#include "acsh.h"

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

    const char *path_env = var_get("PATH");
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
        char candidate[4096];
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

static int pwd_is_current(const char *pwd);

/* Joins dir onto base and removes "." and ".." lexically, as a logical
 * cd does: /a/link/.. is /a, whatever link points to. */
static char *logical_path(const char *base, const char *dir) {
    size_t n = strlen(base) + strlen(dir) + 3;
    char *joined = malloc(n);
    if (dir[0] == '/') snprintf(joined, n, "%s", dir);
    else snprintf(joined, n, "%s/%s", base, dir);
    char *out = malloc(n);
    size_t k = 0;
    out[k++] = '/';
    for (char *part = strtok(joined, "/"); part != NULL; part = strtok(NULL, "/")) {
        if (strcmp(part, ".") == 0) continue;
        if (strcmp(part, "..") == 0) {
            if (k > 1) {
                k--;                                /* drop the trailing '/' */
                while (k > 1 && out[k - 1] != '/') k--;
            }
            continue;
        }
        size_t l = strlen(part);
        memcpy(out + k, part, l);
        k += l;
        out[k++] = '/';
    }
    if (k > 1) k--;
    out[k] = '\0';
    free(joined);
    return out;
}

/* Changes to dir, logically (from $PWD) unless physical; sets $PWD and
 * $OLDPWD on success. */
static int change_dir(const char *dir, int physical) {
    char cwd[4096];
    const char *pwd = var_get("PWD");
    if (!pwd_is_current(pwd)) pwd = getcwd(cwd, sizeof(cwd));
    char *old = strdup(pwd != NULL ? pwd : "");
    char *target = (!physical && pwd != NULL) ? logical_path(pwd, dir) : strdup(dir);
    int r = chdir(target);
    if (r == 0) {
        if (*old) var_set("OLDPWD", old);
        if (physical || pwd == NULL) {
            if (getcwd(cwd, sizeof(cwd)) != NULL) var_set("PWD", cwd);
        } else {
            var_set("PWD", target);
        }
    }
    free(old);
    free(target);
    return r;
}

/* cd [-L | -P] [DIR | -]: no DIR means $HOME, - means $OLDPWD (and
 * prints it). A relative DIR is also looked up in $CDPATH. -P resolves
 * symlinks; the default (-L) keeps the path as typed. */
static int builtin_cd(Command *cmd) {
    int i = 1, physical = 0;
    for (; i < cmd->argc && cmd->argv[i][0] == '-' && cmd->argv[i][1]; i++) {
        if (strcmp(cmd->argv[i], "-P") == 0) physical = 1;
        else if (strcmp(cmd->argv[i], "-L") == 0) physical = 0;
        else if (strcmp(cmd->argv[i], "--") == 0) { i++; break; }
        else break; /* "-" itself, or a directory starting with '-' */
    }
    const char *target = (i < cmd->argc) ? cmd->argv[i] : var_get("HOME");
    int print = 0;
    if (target != NULL && strcmp(target, "-") == 0) {
        target = var_get("OLDPWD");
        print = 1;
        if (target == NULL) {
            shell_error("cd: OLDPWD not set\n");
            return 1;
        }
    }
    if (target == NULL) {
        shell_error("cd: HOME not set\n");
        return 1;
    }
    int ok = 0;
    const char *cdpath = var_get("CDPATH");
    if (target[0] != '/' && strncmp(target, "./", 2) != 0 && strncmp(target, "../", 3) != 0 &&
        strcmp(target, ".") != 0 && strcmp(target, "..") != 0 && cdpath != NULL) {
        char *copy = strdup(cdpath);
        for (char *dir = copy, *next; dir != NULL && !ok; dir = next) {
            next = strchr(dir, ':');
            if (next != NULL) *next++ = '\0';
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", *dir ? dir : ".", target);
            if (change_dir(path, physical) == 0) {
                ok = 1;
                if (*dir) print = 1; /* found through CDPATH: show where */
            }
        }
        free(copy);
    }
    if (!ok && change_dir(target, physical) != 0) {
        shell_error("cd: can't cd to %s: %s\n", target, strerror(errno));
        return 2;
    }
    if (print) printf("%s\n", var_get("PWD"));
    return 0;
}

/* Parses all of `s` as a decimal number >= min for builtin `who`.
 * On failure prints ash's "Illegal number" message and returns -1. */
static int parse_number(const char *who, const char *s, long min, long *out) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || errno != 0 || v < min) {
        shell_error("%s: Illegal number: %s\n", who, s);
        return -1;
    }
    *out = v;
    return 0;
}

/* A usage error in a special builtin (exit, shift, break, continue)
 * ends a non-interactive shell with status 2, as in ash; at the prompt
 * it only fails the command. */
static int special_builtin_error(void) {
    if (!shell_is_interactive()) {
        trap_run_exit();
        exit(2);
    }
    return 2;
}

static int builtin_exit(Command *cmd) {
    long code = env_get_last_status();
    if (cmd->argc > 1 && parse_number("exit", cmd->argv[1], 0, &code) != 0) {
        return special_builtin_error();
    }
    /* Run any `trap ... EXIT` before actually terminating -- exit()
     * jumps straight out of the process and never returns to
     * main()'s own loop, so main() calling trap_run_exit() after its
     * loop ends only covers the EOF/Ctrl+D path, not this one. Both
     * paths out of the shell must honor an EXIT trap. */
    trap_run_exit();
    exit((int)(code & 0xff));
    return 0; /* unreachable */
}

/* The logical current directory ($PWD) if it still names the current
 * directory, else the physical one. */
static int pwd_is_current(const char *pwd) {
    struct stat a, b;
    return pwd != NULL && pwd[0] == '/' && stat(pwd, &a) == 0 && stat(".", &b) == 0 &&
           a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

/* pwd [-L | -P]: -L (default) shows the path as reached through
 * symlinks ($PWD), -P the physical path. */
static int builtin_pwd(Command *cmd) {
    int physical = 0;
    for (int i = 1; i < cmd->argc; i++) {
        if (strcmp(cmd->argv[i], "-P") == 0) physical = 1;
        else if (strcmp(cmd->argv[i], "-L") == 0) physical = 0;
    }
    const char *pwd = var_get("PWD");
    if (!physical && pwd_is_current(pwd)) {
        printf("%s\n", pwd);
        return 0;
    }
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("%s\n", cwd);
        return 0;
    }
    shell_error("pwd: %s\n", strerror(errno));
    return 1;
}

/* jobs [-l | -p]: the job table, current (+) and previous (-) first. */
static int builtin_jobs(Command *cmd) {
    int with_pid = 0, pids_only = 0;
    for (int i = 1; i < cmd->argc; i++) {
        if (strcmp(cmd->argv[i], "-l") == 0) with_pid = 1;
        else if (strcmp(cmd->argv[i], "-p") == 0) pids_only = 1;
        else {
            shell_error("jobs: illegal option %s\n", cmd->argv[i]);
            return 2;
        }
    }
    jobs_print_all(with_pid, pids_only);
    return 0;
}

/* The job an fg/bg operand names (default: the current job). */
static Job *job_operand(Command *cmd, const char *who) {
    if (!job_control_enabled()) {
        shell_error("%s: job control is not enabled (not an interactive shell)\n", who);
        return NULL;
    }
    Job *j = jobs_resolve(cmd->argc > 1 ? cmd->argv[1] : NULL);
    if (j == NULL) {
        shell_error("%s: %s: no such job\n", who, cmd->argc > 1 ? cmd->argv[1] : "current");
    }
    return j;
}

/* fg [%job]: continues a job in the foreground, with the terminal. */
static int builtin_fg(Command *cmd) {
    Job *j = job_operand(cmd, "fg");
    if (j == NULL) {
        return job_control_enabled() ? 1 : 2;
    }
    printf("%s\n", j->cmdline);
    fflush(stdout);

    /* SIGCHLD stays blocked while we wait, so the async handler cannot
     * reap this job's processes and lose their exit status. */
    block_sigchld();
    if (j->state == JOB_DONE) {
        int code = j->last_status; /* finished before fg was typed */
        jobs_remove(j);
        unblock_sigchld();
        return code;
    }
    jobs_mark_running(j->pgid);
    terminal_give(j, j->pgid);
    set_foreground_pgid(j->pgid);
    kill(-j->pgid, SIGCONT);

    int code = 0, stopped = 0;
    while (j->remaining > 0) {
        int status;
        pid_t w = waitpid(-j->pgid, &status, WUNTRACED);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (WIFSTOPPED(status)) {
            jobs_mark_stopped(j->pgid);
            code = 128 + WSTOPSIG(status);
            stopped = 1;
            break;
        }
        jobs_child_exited(w, status);
    }
    set_foreground_pgid(0);
    terminal_take_back(stopped ? j : NULL);
    if (j->state == JOB_DONE) {
        code = j->last_status;
        jobs_remove(j);
    }
    unblock_sigchld();
    return code;
}

/* bg [%job]: continues a stopped job in the background. */
static int builtin_bg(Command *cmd) {
    Job *j = job_operand(cmd, "bg");
    if (j == NULL) {
        return job_control_enabled() ? 1 : 2;
    }
    jobs_mark_running(j->pgid);
    kill(-j->pgid, SIGCONT);
    printf("[%d] %s\n", j->id, j->cmdline);
    return 0;
}

/* `export NAME[=value]...` marks variables for the environment of the
 * commands acsh runs; `readonly NAME[=value]...` makes them read-only.
 * With no names, or -p, they list (`export -p` prints lines that can be
 * run again). Both are special builtins: an error ends a script. */
static int export_or_readonly(Command *cmd, int flag, const char *who) {
    int i = 1, remove = 0;
    if (i < cmd->argc && strcmp(cmd->argv[i], "-p") == 0) i++;
    else if (flag == V_EXPORT && i < cmd->argc && strcmp(cmd->argv[i], "-n") == 0) {
        remove = 1; /* export -n NAME: keep the variable, stop exporting it */
        i++;
    }
    if (i >= cmd->argc) {
        var_print(flag == V_EXPORT ? VARS_EXPORTED : VARS_READONLY);
        return 0;
    }
    for (; i < cmd->argc; i++) {
        char *arg = cmd->argv[i];
        char *eq = strchr(arg, '=');
        char *name = eq ? strndup(arg, (size_t)(eq - arg)) : strdup(arg);
        if (!var_valid_name(name)) {
            shell_error("%s: %s: bad variable name\n", who, name);
            free(name);
            return special_builtin_error();
        }
        if (eq != NULL && var_set(name, eq + 1) != 0) {
            free(name);
            return special_builtin_error();
        }
        if (remove) var_clear_flags(name, V_EXPORT);
        else var_add_flags(name, flag);
        free(name);
    }
    return 0;
}

static int builtin_export(Command *cmd) {
    return export_or_readonly(cmd, V_EXPORT, "export");
}

static int builtin_readonly(Command *cmd) {
    return export_or_readonly(cmd, V_READONLY, "readonly");
}

/* `unset [-v] NAME...` removes variables; `unset -f NAME...` functions. */
static int builtin_unset(Command *cmd) {
    int i = 1, functions = 0;
    for (; i < cmd->argc && cmd->argv[i][0] == '-'; i++) {
        if (strcmp(cmd->argv[i], "-f") == 0) functions = 1;
        else if (strcmp(cmd->argv[i], "-v") == 0) functions = 0;
        else if (strcmp(cmd->argv[i], "--") == 0) { i++; break; }
        else {
            shell_error("unset: illegal option %s\n", cmd->argv[i]);
            return special_builtin_error();
        }
    }
    for (; i < cmd->argc; i++) {
        if (functions) {
            function_undefine(cmd->argv[i]);
        } else if (var_unset(cmd->argv[i]) != 0) {
            return special_builtin_error();
        }
    }
    return 0;
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
                alias_print(arg, val);
            } else {
                shell_error("alias: %s: not found\n", arg);
            }
            continue;
        }
        char *name = strndup(arg, (size_t)(eq - arg));
        if (name == NULL) continue;
        alias_set(name, eq + 1);
        free(name);
    }
    return 0;
}

static int builtin_unalias(Command *cmd) {
    if (cmd->argc < 2) {
        shell_error("unalias: usage: unalias NAME [NAME ...]\n");
        return 1;
    }
    for (int i = 1; i < cmd->argc; i++) {
        alias_unset(cmd->argv[i]);
    }
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
/* umask [-S] [MODE]: MODE is octal (022) or symbolic (u=rwx,g=rx,o=,
 * a-w, g+w), as in ash; -S shows the mask as permissions. */
static int builtin_umask(Command *cmd) {
    int symbolic = 0, i = 1;
    if (i < cmd->argc && strcmp(cmd->argv[i], "-S") == 0) { symbolic = 1; i++; }
    mode_t mask = umask(0);
    umask(mask);
    if (i >= cmd->argc) {
        if (!symbolic) {
            printf("%04o\n", (unsigned)mask);
            return 0;
        }
        mode_t perm = ~mask & 0777;
        const char *who = "ugo";
        for (int w = 0; w < 3; w++) {
            int bits = (perm >> (6 - 3 * w)) & 7;
            printf("%c=%s%s%s%s", who[w], (bits & 4) ? "r" : "", (bits & 2) ? "w" : "",
                   (bits & 1) ? "x" : "", w < 2 ? "," : "\n");
        }
        return 0;
    }
    const char *m = cmd->argv[i];
    if (*m >= '0' && *m <= '7') {
        char *end;
        long mode = strtol(m, &end, 8);
        if (*end != '\0' || mode < 0 || mode > 0777) {
            shell_error("umask: illegal mode: %s\n", m);
            return 2;
        }
        umask((mode_t)mode);
        return 0;
    }
    mode_t perm = ~mask & 0777;
    while (*m) {
        mode_t who = 0;
        for (; *m && strchr("ugoa", *m); m++) {
            who |= (*m == 'u') ? 0700 : (*m == 'g') ? 0070 : (*m == 'o') ? 0007 : 0777;
        }
        if (who == 0) who = 0777;
        char op = *m;
        if (op != '=' && op != '+' && op != '-') {
            shell_error("umask: illegal mode: %s\n", cmd->argv[i]);
            return 2;
        }
        m++;
        mode_t bits = 0;
        for (; *m && strchr("rwx", *m); m++) {
            bits |= (*m == 'r') ? 0444 : (*m == 'w') ? 0222 : 0111;
        }
        bits &= who;
        if (op == '=') perm = (perm & ~who) | bits;
        else if (op == '+') perm |= bits;
        else perm &= ~bits;
        if (*m == ',') m++;
        else if (*m) {
            shell_error("umask: illegal mode: %s\n", cmd->argv[i]);
            return 2;
        }
    }
    umask(~perm & 0777);
    return 0;
}

/* `type NAME...` reports, for each NAME, whether it's an alias, a
 * shell function, a builtin, or found on $PATH (in that priority
 * order, matching how acsh itself would actually resolve and run
 * it), or reports "not found". Does not run anything -- purely
 * informational, which is the whole point of `type` as distinct from
 * just trying to run NAME and seeing what happens. */
static int builtin_type(Command *cmd) {
    int paths_only = 0, first = 1;
    if (cmd->argc > 1 && strcmp(cmd->argv[1], "-p") == 0) {
        paths_only = 1; /* type -p: only the path of external commands */
        first = 2;
    }
    if (cmd->argc <= first) {
        shell_error("type: usage: type [-p] NAME [NAME ...]\n");
        return 1;
    }

    int any_not_found = 0;
    for (int i = first; i < cmd->argc; i++) {
        const char *name = cmd->argv[i];
        if (paths_only) {
            char *path = (alias_get(name) || function_is_defined(name) || is_builtin_name(name))
                         ? NULL : find_in_path(name);
            if (path != NULL) printf("%s\n", path);
            free(path);
            continue;
        }

        const char *alias_val = alias_get(name);
        if (alias_val != NULL) {
            printf("%s is an alias for %s\n", name, alias_val);
            continue;
        }
        if (function_is_defined(name)) {
            printf("%s is a shell function\n", name);
            continue;
        }
        if (is_builtin_name(name)) {
            printf("%s is a %sshell builtin\n", name, is_special_builtin(name) ? "special " : "");
            continue;
        }
        char *path = find_in_path(name);
        if (path != NULL) {
            printf("%s is %s\n", name, path);
            free(path);
            continue;
        }
        shell_error("type: %s: not found\n", name);
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
static int builtin_command_run(Command *cmd);

/* command [-p] [-v | -V] NAME [ARGS]: -p looks NAME up in the
 * standard PATH (confstr _CS_PATH) instead of $PATH. */
static int builtin_command(Command *cmd) {
    if (cmd->argc > 2 && strcmp(cmd->argv[1], "-p") == 0) {
        char std[1024];
        size_t n = confstr(_CS_PATH, std, sizeof(std));
        if (n == 0 || n > sizeof(std)) snprintf(std, sizeof(std), "/usr/bin:/bin:/usr/sbin:/sbin");
        VarSave saved = var_save("PATH");
        int ro = var_flags("PATH") & V_READONLY;
        if (!ro) var_set("PATH", std);
        char *flag = cmd->argv[1];
        Command rest = { cmd->argv + 1, cmd->argc - 1, 0 };
        rest.argv[0] = "command";
        int status = builtin_command_run(&rest);
        cmd->argv[1] = flag;
        if (!ro) var_restore(&saved);
        else { free(saved.name); free(saved.value); }
        return status;
    }
    return builtin_command_run(cmd);
}

static int builtin_command_run(Command *cmd) {
    if (cmd->argc <= 1) {
        shell_error("command: usage: command [-p] [-v | -V] NAME [ARGS ...]\n");
        return 1;
    }

    if (strcmp(cmd->argv[1], "-V") == 0) {
        /* command -V NAME: describes NAME as `type` does */
        char *saved = cmd->argv[1];
        Command t = { cmd->argv + 1, cmd->argc - 1, 0 };
        t.argv[0] = "type";
        int status = builtin_type(&t);
        cmd->argv[1] = saved;
        return status;
    }
    if (strcmp(cmd->argv[1], "-v") == 0) {
        if (cmd->argc <= 2) {
            shell_error("command: -v: usage: command -v NAME\n");
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
    shifted.argv = cmd->argv + 1;   /* still NULL-terminated; nothing copied or freed */
    shifted.argv_cap = 0;

    int exit_status = 0;
    if (try_run_builtin(&shifted, &exit_status)) {
        return exit_status;
    }

    /* not a builtin: run it as an external command, skipping any
     * function of the same name */
    return exec_external(shifted.argv);
}

static int builtin_exec(Command *cmd) {
    if (cmd->argc <= 1) {
        return 0; /* bare `exec`: no-op, see note above */
    }

    /* argv[1..] becomes the new process's argv[0..] */
    execvp(cmd->argv[1], cmd->argv + 1);

    /* only reached if execvp failed */
    if (errno == ENOENT) {
        shell_error("exec: %s: command not found\n", cmd->argv[1]);
    } else {
        shell_error("exec: %s: %s\n", cmd->argv[1], strerror(errno));
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
 * treated as a literal argument. The text goes through the normal
 * parser and executor (run_string), so it can contain anything a
 * script line can. */
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
/* trap [ACTION] SIGNAL...: ACTION is a command, '' to ignore, or - to
 * restore the default (also when the first operand is a number, as
 * POSIX says). SIGNAL: a name (TERM, SIGTERM) or number, EXIT/0, ERR.
 * No operands, or -p: list the traps. */
static int builtin_trap(Command *cmd) {
    int i = 1;
    if (i < cmd->argc && strcmp(cmd->argv[i], "--") == 0) i++;
    if (i >= cmd->argc || (strcmp(cmd->argv[i], "-p") == 0 && i + 1 == cmd->argc)) {
        trap_print();
        return 0;
    }
    if (strcmp(cmd->argv[i], "-p") == 0) {
        /* trap -p SIGNAL...: those signals' traps (POSIX.1-2024) */
        int status = 0;
        for (i++; i < cmd->argc; i++) {
            if (trap_print_one(cmd->argv[i]) != 0) {
                shell_error("trap: %s: invalid signal specification\n", cmd->argv[i]);
                status = 1;
            }
        }
        return status;
    }
    const char *action = cmd->argv[i];
    if (strcmp(action, "-") == 0) {
        action = NULL;
        i++;
    } else if (isdigit((unsigned char)action[0]) && strspn(action, "0123456789") == strlen(action)) {
        action = NULL; /* trap N...: reset */
    } else {
        i++;
    }
    if (i >= cmd->argc) {
        shell_error("trap: usage: trap [ACTION] SIGNAL...\n");
        return 2;
    }
    int status = 0;
    for (; i < cmd->argc; i++) {
        if (trap_set(cmd->argv[i], action) != 0) {
            shell_error("trap: %s: invalid signal specification\n", cmd->argv[i]);
            status = 1;
        }
    }
    return status;
}

/* Option letters `set` accepts, and the long names of `set -o`, in
 * ash's order. Letters P (pipefail), I, b, E and V are internal: they
 * have no short form. Behavior lives where each is checked: e, x, P, C
 * in exec.c; u, f in expand.c; a in vars.c; n, v in main.c. */
#define SET_OPTIONS "abefnuvxBC"

static const struct { const char *name; char letter; } LONG_OPTIONS[] = {
    { "errexit", 'e' }, { "noglob", 'f' }, { "ignoreeof", 'I' }, { "monitor", 'm' },
    { "noexec", 'n' }, { "xtrace", 'x' }, { "verbose", 'v' }, { "noclobber", 'C' },
    { "allexport", 'a' }, { "notify", 'b' }, { "nounset", 'u' }, { "errtrace", 'E' },
    { "vi", 'V' }, { "pipefail", 'P' }, { "braceexpand", 'B' }, { "suggest", 'S' }, { NULL, 0 }
};

/* set -o: "name   on/off"; set +o: commands that recreate the settings. */
static void list_long_options(int as_commands) {
    for (int i = 0; LONG_OPTIONS[i].name != NULL; i++) {
        int on = shell_option(LONG_OPTIONS[i].letter);
        if (as_commands) {
            printf("set %co %s\n", on ? '-' : '+', LONG_OPTIONS[i].name);
        } else {
            printf("%-15s %s\n", LONG_OPTIONS[i].name, on ? "on" : "off");
        }
    }
}

static int builtin_set(Command *cmd) {
    if (cmd->argc == 1) {
        var_print(VARS_ALL); /* bare `set`: every variable */
        return 0;
    }
    int i = 1;
    for (; i < cmd->argc; i++) {
        const char *a = cmd->argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if ((a[0] != '-' && a[0] != '+') || a[1] == '\0') {
            break; /* first operand: the new positional parameters */
        }
        int on = (a[0] == '-');
        for (const char *c = a + 1; *c; c++) {
            if (*c == 'o') {
                if (i + 1 >= cmd->argc) {
                    list_long_options(!on);
                    continue;
                }
                const char *name = cmd->argv[++i];
                int found = 0;
                for (int k = 0; LONG_OPTIONS[k].name != NULL; k++) {
                    if (strcmp(LONG_OPTIONS[k].name, name) == 0) {
                        shell_set_option(LONG_OPTIONS[k].letter, on);
                        found = 1;
                    }
                }
                if (!found) {
                    shell_error("set: illegal option -o %s\n", name);
                    return special_builtin_error();
                }
                continue;
            }
            if (strchr(SET_OPTIONS, *c) == NULL) {
                shell_error("set: illegal option %c%c\n", a[0], *c);
                return special_builtin_error();
            }
            shell_set_option(*c, on);
        }
    }
    if (i < cmd->argc || (i > 1 && strcmp(cmd->argv[i - 1], "--") == 0)) {
        env_set_positional_params(cmd->argc - i, cmd->argv + i);
    }
    return 0;
}

static int builtin_local(Command *cmd) {
    if (cmd->argc <= 1) {
        shell_error("local: usage: local NAME[=value] [NAME[=value] ...]\n");
        return 1;
    }

    int status = 0;
    for (int i = 1; i < cmd->argc; i++) {
        char *arg = cmd->argv[i];
        char *eq = strchr(arg, '=');

        if (eq != NULL) {
            char *name = strndup(arg, (size_t)(eq - arg));
            if (name == NULL) continue;
            if (env_declare_local(name, eq + 1) != 0) {
                shell_error("local: %s: not inside a function\n", name);
                status = 1;
            }
            free(name);
        } else {
            if (env_declare_local(arg, NULL) != 0) {
                shell_error("local: %s: not inside a function\n", arg);
                status = 1;
            }
        }
    }
    return status;
}

static int builtin_shift(Command *cmd) {
    long n = 1;
    if (cmd->argc > 1 && parse_number("shift", cmd->argv[1], 0, &n) != 0) {
        return special_builtin_error();
    }
    if (env_shift_positional_params((int)n) != 0) {
        shell_error("shift: can't shift %ld (only %d positional parameters)\n",
                n, env_positional_count());
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

    int status = run_string(joined);
    free(joined);
    return status;
}

/* read [-r] [-p PROMPT] [-t SECS] [-n N] [-s] [-u FD] [-d DELIM] [NAME...]
 *
 * Reads one line a byte at a time (never ahead of it: the rest of the
 * input belongs to whatever runs next). Without -r a backslash keeps
 * the next character literal and a trailing backslash joins the next
 * line. The line is split on $IFS: each NAME gets one field, the last
 * gets the rest. With no NAME the whole line goes to $REPLY. Status 1
 * at end of input or on timeout (the variables are still set). */
static int builtin_read(Command *cmd) {
    int raw = 0, nchars = -1, silent = 0, fd = STDIN_FILENO, i = 1;
    char delim = '\n';
    const char *prompt = NULL;
    double timeout = -1;
    for (; i < cmd->argc && cmd->argv[i][0] == '-' && cmd->argv[i][1]; i++) {
        const char *o = cmd->argv[i];
        if (strcmp(o, "--") == 0) { i++; break; }
        for (const char *c = o + 1; *c; c++) {
            if (*c == 'r') { raw = 1; continue; }
            if (*c == 's') { silent = 1; continue; }
            if (!strchr("ptnud", *c)) {
                shell_error("read: illegal option -%c\n", *c);
                return 2;
            }
            const char *arg = c[1] ? c + 1 : (i + 1 < cmd->argc ? cmd->argv[++i] : NULL);
            if (arg == NULL) {
                shell_error("read: -%c needs an argument\n", *c);
                return 2;
            }
            if (*c == 'p') prompt = arg;
            else if (*c == 't') timeout = strtod(arg, NULL);
            else if (*c == 'n') nchars = atoi(arg);
            else if (*c == 'u') fd = atoi(arg);
            else delim = *arg;
            break;
        }
    }
    if (prompt != NULL && isatty(fd)) {
        fputs(prompt, stderr);
    }
    struct termios saved;
    int restore_tty = silent && isatty(fd) && tcgetattr(fd, &saved) == 0;
    if (restore_tty) {
        struct termios quiet = saved;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(fd, TCSANOW, &quiet);
    }

    /* the line, with a flag per character: escaped (not split on) */
    size_t len = 0, cap = 64;
    char *line = malloc(cap), *esc = malloc(cap);
    int status = 1;
    for (;;) {
        if (nchars >= 0 && (long)len >= nchars) { status = 0; break; }
        if (timeout >= 0) {
            struct pollfd pfd = { fd, POLLIN, 0 };
            int ready = poll(&pfd, 1, (int)(timeout * 1000));
            if (ready <= 0) break; /* timed out */
        }
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0 && errno == EINTR) {
            if (trap_pending_signal() != 0) break;
            continue;
        }
        if (n <= 0) break;
        int escaped = 0;
        if (!raw && c == '\\') {
            n = read(fd, &c, 1);
            if (n <= 0) break;
            if (c == '\n') continue; /* line continuation */
            escaped = 1;
        } else if (c == delim) {
            status = 0;
            break;
        }
        if (len + 2 > cap) {
            cap *= 2;
            line = realloc(line, cap);
            esc = realloc(esc, cap);
        }
        line[len] = c;
        esc[len] = (char)escaped;
        len++;
    }
    line[len] = '\0';
    if (restore_tty) {
        tcsetattr(fd, TCSANOW, &saved);
        if (silent) fputc('\n', stderr);
    }
    int nvars = cmd->argc - i;
    if (nvars == 0) {
        if (var_set("REPLY", line) != 0) status = 2;
        free(line);
        free(esc);
        return status;
    }
    const char *ifs = var_get("IFS");
    if (ifs == NULL) ifs = " \t\n";
#define IS_IFS(k) (!esc[k] && line[k] != '\0' && strchr(ifs, line[k]) != NULL)
#define IS_WS(k)  (IS_IFS(k) && strchr(" \t\n", line[k]) != NULL)
    size_t k = 0;
    while (k < len && IS_WS(k)) k++;                  /* leading IFS whitespace */
    for (int v = 0; v < nvars; v++) {
        const char *name = cmd->argv[i + v];
        size_t start = k, end;
        if (v == nvars - 1) {
            end = len;                                /* the last name: the rest */
            while (end > start && IS_WS(end - 1)) end--;
        } else {
            while (k < len && !IS_IFS(k)) k++;
            end = k;
            while (k < len && IS_WS(k)) k++;          /* skip one separator */
            if (k < len && IS_IFS(k) && !IS_WS(k)) {
                k++;
                while (k < len && IS_WS(k)) k++;
            }
        }
        char *field = strndup(line + start, end - start);
        if (var_set(name, field) != 0) status = 2;
        free(field);
    }
#undef IS_IFS
#undef IS_WS
    free(line);
    free(esc);
    return status;
}

static int builtin_source(Command *cmd) {
    if (cmd->argc < 2) {
        shell_error("source: usage: source FILE\n");
        return 1;
    }
    return run_script_file(cmd->argv[1]);
}

/* `return [n]`: leaves the current function with status n (default:
 * the last command's status), taken modulo 256 as in ash. Outside any
 * function a script ends with that status, as in ash. */
static int builtin_return(Command *cmd) {
    long n = env_get_last_status();
    if (cmd->argc > 1 && parse_number("return", cmd->argv[1], 0, &n) != 0) {
        return special_builtin_error();
    }
    n &= 0xff;
    if (env_get_call_depth() == 0 && !control_flow_in_file()) {
        if (!shell_is_interactive()) {
            trap_run_exit();
            exit((int)n);
        }
        shell_error("return: not in a function\n");
        return 1;
    }
    control_flow_signal_return((int)n);
    return (int)n;
}

static int builtin_break_or_continue(Command *cmd, int is_continue) {
    if (control_flow_in_loop_depth() <= 0) {
        /* Matches real shells: not inside any loop is a warning, not
         * a hard error -- the shell keeps running normally. */
        shell_error("%s: only meaningful inside a loop\n",
                is_continue ? "continue" : "break");
        return 0;
    }

    long level = 1;
    if (cmd->argc > 1 &&
        parse_number(is_continue ? "continue" : "break", cmd->argv[1], 1, &level) != 0) {
        return special_builtin_error();
    }

    control_flow_signal_loop(is_continue, (int)level);
    return 0;
}

/* ---- small builtins ---- */

static int builtin_colon(Command *cmd) { (void)cmd; return 0; }
static int builtin_false(Command *cmd) { (void)cmd; return 1; }

/* getopts OPTSTRING NAME [ARG...]: the next option from ARGs (default:
 * the positional parameters) into NAME, its argument into OPTARG, the
 * next index into OPTIND. A leading ':' in OPTSTRING selects silent
 * error reporting. Returns 1 when the options are exhausted. */
static int builtin_getopts(Command *cmd) {
    static int pos = 1;          /* position inside a cluster like -abc */
    static long last_index = 1;  /* OPTIND we last set; any other value restarts */
    if (cmd->argc < 3) {
        shell_error("getopts: usage: getopts OPTSTRING NAME [ARG...]\n");
        return 2;
    }
    const char *optstring = cmd->argv[1];
    const char *name = cmd->argv[2];
    int silent = (optstring[0] == ':');
    if (silent) optstring++;

    int argc = cmd->argc > 3 ? cmd->argc - 3 : env_positional_count();
    const char *optind_s = var_get("OPTIND");
    long index = optind_s != NULL ? strtol(optind_s, NULL, 10) : 1;
    if (index < 1) index = 1;
    if (index != last_index) pos = 1;

#define ARG(i) (cmd->argc > 3 ? cmd->argv[3 + (i) - 1] : env_positional(i))
    char optbuf[2] = { 0, 0 };
    int status = 0;
    if (index > argc || ARG(index)[0] != '-' || ARG(index)[1] == '\0') {
        var_set(name, "?");
        var_unset("OPTARG");
        status = 1;
    } else if (strcmp(ARG(index), "--") == 0) {
        index++;
        var_set(name, "?");
        var_unset("OPTARG");
        status = 1;
    } else {
        const char *arg = ARG(index);
        char c = arg[pos++];
        const char *spec = (c != ':') ? strchr(optstring, c) : NULL;
        optbuf[0] = c;
        if (spec == NULL) {
            if (silent) {
                var_set(name, "?");
                var_set("OPTARG", optbuf);
            } else {
                fprintf(stderr, "Illegal option -%c\n", c);
                var_set(name, "?");
                var_unset("OPTARG");
            }
        } else if (spec[1] == ':') {
            if (arg[pos] != '\0') {
                var_set("OPTARG", arg + pos);
                index++;
                pos = 1;
            } else if (index < argc) {
                var_set("OPTARG", ARG(index + 1));
                index += 2;
                pos = 1;
            } else if (silent) {
                var_set(name, ":");
                var_set("OPTARG", optbuf);
                index++;
                pos = 1;
                goto done;
            } else {
                fprintf(stderr, "No arg for -%c option\n", c);
                var_set(name, "?");
                var_unset("OPTARG");
                index++;
                pos = 1;
                goto done;
            }
            var_set(name, optbuf);
        } else {
            var_set(name, optbuf);
            var_unset("OPTARG");
        }
        if (pos > 1 && arg[pos] == '\0') {
            index++;
            pos = 1;
        }
    }
done:
#undef ARG
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld", index);
        var_set("OPTIND", buf);
        last_index = index;
    }
    return status;
}

/* Waits for every process of a job; returns the exit code of its last
 * process and drops the job. A job killed by a signal (other than
 * SIGINT or SIGPIPE) is reported, as ash does. */
static int wait_for_job(Job *j) {
    block_sigchld();
    for (int k = 0; k < j->num_pids && j->remaining > 0; k++) {
        int st;
        pid_t w;
        do {
            w = waitpid(j->pids[k], &st, 0);
            int sig = trap_pending_signal();
            if (w < 0 && errno == EINTR && sig != 0) {
                /* a trapped signal arrived: `wait` returns 128+N and
                 * the trap runs; the job keeps running */
                unblock_sigchld();
                return 128 + sig;
            }
        } while (w < 0 && errno == EINTR && !shell_interrupted());
        if (w == j->pids[k]) {
            jobs_child_exited(w, st);
        }
    }
    int status = j->last_status;
    jobs_remove(j);
    unblock_sigchld();
    if (status > 128 && status - 128 != SIGINT && status - 128 != SIGPIPE) {
        fprintf(stderr, "%s\n", strsignal(status - 128));
    }
    return status;
}

/* wait [PID | %JOB ...]: no operand waits for every background job. */
static int builtin_wait(Command *cmd) {
    if (cmd->argc < 2) {
        for (int i = 0; i < jobs_capacity(); i++) {
            Job *j = jobs_at(i);
            if (j != NULL && j->state != JOB_STOPPED) {
                int st = wait_for_job(j);
                if (st > 128 && trap_pending_signal() == st - 128) return st;
            }
        }
        return 0;
    }
    int status = 0;
    for (int i = 1; i < cmd->argc; i++) {
        const char *a = cmd->argv[i];
        Job *j = NULL;
        if (a[0] == '%') {
            j = jobs_resolve(a);
        } else {
            long pid;
            if (parse_number("wait", a, 1, &pid) != 0) return 2;
            j = jobs_find_by_pid((pid_t)pid);
        }
        status = (j != NULL) ? wait_for_job(j) : 127; /* not a child of ours */
    }
    return status;
}

/* kill [-s SIGNAL | -SIGNAL] PID|%JOB...   kill -l [STATUS] */
static int builtin_kill(Command *cmd) {
    int i = 1, sig = SIGTERM;
    if (i < cmd->argc && strcmp(cmd->argv[i], "-l") == 0) {
        if (i + 1 >= cmd->argc) {
            signal_list();
            return 0;
        }
        int status = 0;
        for (i++; i < cmd->argc; i++) {
            int n = atoi(cmd->argv[i]);
            const char *name = signal_name(n > 128 ? n - 128 : n);
            if (name != NULL) printf("%s\n", name);
            else { shell_error("kill: %s: invalid signal\n", cmd->argv[i]); status = 1; }
        }
        return status;
    }
    if (i < cmd->argc && (strcmp(cmd->argv[i], "-s") == 0 || strcmp(cmd->argv[i], "-n") == 0)) {
        if (i + 1 >= cmd->argc || (sig = signal_number(cmd->argv[i + 1])) < 0) {
            shell_error("kill: bad signal %s\n", i + 1 < cmd->argc ? cmd->argv[i + 1] : "");
            return 1;
        }
        i += 2;
    } else if (i < cmd->argc && cmd->argv[i][0] == '-' && cmd->argv[i][1] != '\0' &&
               strcmp(cmd->argv[i], "--") != 0) {
        if ((sig = signal_number(cmd->argv[i] + 1)) < 0) {
            shell_error("kill: bad signal %s\n", cmd->argv[i] + 1);
            return 1;
        }
        i++;
    }
    if (i < cmd->argc && strcmp(cmd->argv[i], "--") == 0) i++;
    if (i >= cmd->argc) {
        shell_error("kill: usage: kill [-s SIGNAL | -SIGNAL] PID|%%JOB...\n");
        return 1;
    }
    int status = 0;
    for (; i < cmd->argc; i++) {
        const char *a = cmd->argv[i];
        pid_t target;
        if (a[0] == '%') {
            Job *j = jobs_resolve(a);
            if (j == NULL) {
                shell_error("kill: %s: no such job\n", a);
                status = 1;
                continue;
            }
            if (!j->own_group) {
                /* started without job control: no process group of its
                 * own, so signal each of its processes */
                for (int k = 0; k < j->num_pids; k++) kill(j->pids[k], sig);
                continue;
            }
            target = -j->pgid;
        } else {
            char *end;
            long n = strtol(a, &end, 10);
            if (*a == '\0' || *end != '\0') {
                shell_error("kill: %s: invalid pid\n", a);
                status = 1;
                continue;
            }
            target = (pid_t)n;
        }
        if (kill(target, sig) != 0) {
            shell_error("kill: can't kill pid %s: %s\n", a, strerror(errno));
            status = 1;
        }
    }
    return status;
}

/* ulimit [-HS] [-a | -cdefilmnqrstuvx [LIMIT]]: shows or sets resource
 * limits (default -f), with ash's units and -a layout. */
static const struct { char opt; int res; int unit; const char *desc; } LIMITS[] = {
    { 'c', RLIMIT_CORE, 512, "core file size (blocks)" },
    { 'd', RLIMIT_DATA, 1024, "data seg size (kb)" },
    { 'e', RLIMIT_NICE, 1, "scheduling priority" },
    { 'f', RLIMIT_FSIZE, 512, "file size (blocks)" },
    { 'i', RLIMIT_SIGPENDING, 1, "pending signals" },
    { 'l', RLIMIT_MEMLOCK, 1024, "max locked memory (kb)" },
    { 'm', RLIMIT_RSS, 1024, "max memory size (kb)" },
    { 'n', RLIMIT_NOFILE, 1, "open files" },
    { 'q', RLIMIT_MSGQUEUE, 1, "POSIX message queues (bytes)" },
    { 'r', RLIMIT_RTPRIO, 1, "real-time priority" },
    { 's', RLIMIT_STACK, 1024, "stack size (kb)" },
    { 't', RLIMIT_CPU, 1, "cpu time (seconds)" },
    { 'u', RLIMIT_NPROC, 1, "max user processes" },
    { 'v', RLIMIT_AS, 1024, "virtual memory (kb)" },
    { 'x', RLIMIT_LOCKS, 1, "file locks" },
    { 0, 0, 0, NULL }
};

static void print_limit(rlim_t v, int unit) {
    if (v == RLIM_INFINITY) printf("unlimited\n");
    else printf("%llu\n", (unsigned long long)(v / (rlim_t)unit));
}

static int builtin_ulimit(Command *cmd) {
    int hard = 0, soft = 0, all = 0;
    int idx = 3; /* -f, the default */
    int i = 1;
    for (; i < cmd->argc && cmd->argv[i][0] == '-' && cmd->argv[i][1]; i++) {
        for (const char *c = cmd->argv[i] + 1; *c; c++) {
            if (*c == 'H') hard = 1;
            else if (*c == 'S') soft = 1;
            else if (*c == 'a') all = 1;
            else {
                int k;
                for (k = 0; LIMITS[k].opt && LIMITS[k].opt != *c; k++) {}
                if (!LIMITS[k].opt) {
                    shell_error("ulimit: illegal option -%c\n", *c);
                    return 2;
                }
                idx = k;
            }
        }
    }
    if (all) {
        for (int k = 0; LIMITS[k].opt; k++) {
            struct rlimit rl;
            getrlimit(LIMITS[k].res, &rl);
            printf("%-32s(-%c) ", LIMITS[k].desc, LIMITS[k].opt);
            print_limit(hard ? rl.rlim_max : rl.rlim_cur, LIMITS[k].unit);
        }
        return 0;
    }
    struct rlimit rl;
    if (getrlimit(LIMITS[idx].res, &rl) != 0) {
        shell_error("ulimit: %s\n", strerror(errno));
        return 1;
    }
    if (i >= cmd->argc) {
        print_limit(hard ? rl.rlim_max : rl.rlim_cur, LIMITS[idx].unit);
        return 0;
    }
    rlim_t v;
    if (strcmp(cmd->argv[i], "unlimited") == 0) {
        v = RLIM_INFINITY;
    } else {
        char *end;
        unsigned long long n = strtoull(cmd->argv[i], &end, 10);
        if (*cmd->argv[i] == '\0' || *end != '\0') {
            shell_error("ulimit: bad number %s\n", cmd->argv[i]);
            return 2;
        }
        v = (rlim_t)n * (rlim_t)LIMITS[idx].unit;
    }
    if (hard || !soft) rl.rlim_max = v;   /* neither -H nor -S: both */
    if (soft || !hard) rl.rlim_cur = v;
    if (setrlimit(LIMITS[idx].res, &rl) != 0) {
        shell_error("ulimit: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

/* hash [-r] [NAME...]: remembers where commands are found in $PATH and
 * lists them; -r forgets them. */
static char **hashed = NULL;
static int nhashed = 0, hashed_cap = 0;

static int builtin_hash(Command *cmd) {
    if (cmd->argc == 1) {
        for (int i = 0; i < nhashed; i++) printf("%s\n", hashed[i]);
        return 0;
    }
    int status = 0;
    for (int i = 1; i < cmd->argc; i++) {
        if (strcmp(cmd->argv[i], "-r") == 0) {
            for (int k = 0; k < nhashed; k++) free(hashed[k]);
            nhashed = 0;
            continue;
        }
        if (is_builtin_name(cmd->argv[i]) || function_is_defined(cmd->argv[i])) continue;
        char *path = find_in_path(cmd->argv[i]);
        if (path == NULL) {
            shell_error("hash: %s: not found\n", cmd->argv[i]);
            status = 1;
            continue;
        }
        int known = 0;
        for (int k = 0; k < nhashed; k++) if (strcmp(hashed[k], path) == 0) known = 1;
        if (!known && nhashed == hashed_cap) {
            int ncap = hashed_cap ? hashed_cap * 2 : 32;
            char **bigger = realloc(hashed, (size_t)ncap * sizeof(char *));
            if (bigger != NULL) { hashed = bigger; hashed_cap = ncap; }
        }
        if (!known && nhashed < hashed_cap) hashed[nhashed++] = path;
        else free(path);
    }
    return status;
}

/* times: user and system time of the shell, then of its children. */
static int builtin_times(Command *cmd) {
    (void)cmd;
    struct tms t;
    times(&t);
    double tick = (double)sysconf(_SC_CLK_TCK);
    clock_t v[4] = { t.tms_utime, t.tms_stime, t.tms_cutime, t.tms_cstime };
    for (int i = 0; i < 4; i += 2) {
        double u = (double)v[i] / tick, s = (double)v[i + 1] / tick;
        printf("%dm%.3fs %dm%.3fs\n", (int)(u / 60), u - 60 * (int)(u / 60),
               (int)(s / 60), s - 60 * (int)(s / 60));
    }
    return 0;
}

/* let EXPR...: arithmetic; status 1 if the last value is 0. */
static int builtin_let(Command *cmd) {
    if (cmd->argc < 2) {
        shell_error("let: usage: let EXPRESSION...\n");
        return 2;
    }
    long v = 0;
    for (int i = 1; i < cmd->argc; i++) {
        if (arith_evaluate(cmd->argv[i], &v) != 0) return 2;
    }
    return v == 0 ? 1 : 0;
}

/* ---- the builtin table ---- */

static int builtin_break(Command *cmd)    { return builtin_break_or_continue(cmd, 0); }
static int builtin_continue(Command *cmd) { return builtin_break_or_continue(cmd, 1); }
static int builtin_history_cmd(Command *cmd) { return history_builtin(cmd->argc, cmd->argv); }
static int builtin_fc(Command *cmd) { return fc_builtin(cmd->argc, cmd->argv); }

/* Every builtin, once: dispatch, `type`, `command -v` and `hash` all
 * use this table. */
static int builtin_help(Command *cmd);

static const struct { const char *name; int (*run)(Command *); } BUILTINS[] = {
    { ":", builtin_colon }, { ".", builtin_source }, { "[", builtin_test }, { "[[", builtin_test },
    { "alias", builtin_alias }, { "bg", builtin_bg }, { "break", builtin_break },
    { "cd", builtin_cd }, { "chdir", builtin_cd }, { "command", builtin_command }, { "continue", builtin_continue },
    { "echo", builtin_echo }, { "eval", builtin_eval }, { "exec", builtin_exec },
    { "exit", builtin_exit }, { "export", builtin_export }, { "false", builtin_false }, { "fc", builtin_fc },
    { "fg", builtin_fg }, { "getopts", builtin_getopts }, { "hash", builtin_hash },
    { "help", builtin_help },
    { "history", builtin_history_cmd }, { "jobs", builtin_jobs }, { "kill", builtin_kill },
    { "let", builtin_let }, { "local", builtin_local }, { "printf", builtin_printf },
    { "pwd", builtin_pwd }, { "read", builtin_read }, { "readonly", builtin_readonly },
    { "return", builtin_return }, { "set", builtin_set }, { "shift", builtin_shift },
    { "source", builtin_source }, { "test", builtin_test }, { "times", builtin_times },
    { "trap", builtin_trap }, { "true", builtin_colon }, { "type", builtin_type },
    { "ulimit", builtin_ulimit }, { "umask", builtin_umask }, { "unalias", builtin_unalias },
    { "unset", builtin_unset }, { "wait", builtin_wait },
    { NULL, NULL }
};

/* help: the builtin commands, in the layout ash uses. */
static int builtin_help(Command *cmd) {
    (void)cmd;
    printf("Built-in commands:\n------------------\n");
    int col = 0;
    for (int i = 0; BUILTINS[i].name != NULL; i++) {
        int len = (int)strlen(BUILTINS[i].name) + 1;
        if (col == 0) { putchar('\t'); col = 8; }
        else if (col + len > 72) { printf("\n\t"); col = 8; }
        else { putchar(' '); col++; }
        fputs(BUILTINS[i].name, stdout);
        col += len - 1;
    }
    putchar('\n');
    return 0;
}

static int (*find_builtin(const char *name))(Command *) {
    for (int i = 0; BUILTINS[i].name != NULL; i++) {
        if (strcmp(BUILTINS[i].name, name) == 0) return BUILTINS[i].run;
    }
    return NULL;
}

/* Every builtin name, for Tab completion. */
void builtin_each(void (*fn)(const char *name, void *ctx), void *ctx) {
    for (int i = 0; BUILTINS[i].name != NULL; i++) fn(BUILTINS[i].name, ctx);
}

int is_builtin_name(const char *name) {
    return find_builtin(name) != NULL;
}

/* POSIX special builtins: assignments before them stay set, and their
 * errors end a non-interactive shell. */
int is_special_builtin(const char *name) {
    static const char *names[] = { "break", ":", "continue", ".", "eval", "exec", "exit",
                                   "export", "readonly", "return", "set", "shift", "times",
                                   "trap", "unset", NULL };
    for (int i = 0; names[i] != NULL; i++) {
        if (strcmp(name, names[i]) == 0) return 1;
    }
    return 0;
}

int try_run_builtin(Command *cmd, int *exit_status) {
    if (cmd->argc == 0 || cmd->argv[0] == NULL) {
        return 0;
    }
    int (*run)(Command *) = find_builtin(cmd->argv[0]);
    if (run == NULL) {
        return 0;
    }
    *exit_status = run(cmd);
    return 1;
}
