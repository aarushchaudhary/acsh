#define _POSIX_C_SOURCE 200809L  /* getcwd, getline, fdopen under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "acsh.h"
#include "syntax.h"

/* 1 for the interactive shell (prompt, job notices, default aliases);
 * 0 for a script and in every forked subshell. */
static int interactive = 0;

int shell_is_interactive(void) {
    return interactive;
}

void shell_mark_subshell(void) {
    interactive = 0;
}


/* Reads input lines straight from a file descriptor through acsh's own
 * buffer. stdio is not used for shell input: a forked child that
 * flushes or closes an inherited input FILE moves the shared file
 * offset, and the shell would then re-read part of its script. */
typedef struct {
    int    fd;
    char   buf[4096];
    size_t pos, len;
} LineReader;

/* One whole line of any length, '\n' included (heap, caller frees), or
 * NULL at end of input. */
static char *reader_line(LineReader *r) {
    char *line = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (r->pos == r->len) {
            ssize_t n = read(r->fd, r->buf, sizeof(r->buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            r->pos = 0;
            r->len = (size_t)n;
        }
        char *nl = memchr(r->buf + r->pos, '\n', r->len - r->pos);
        size_t take = nl ? (size_t)(nl - (r->buf + r->pos)) + 1 : r->len - r->pos;
        if (len + take + 1 > cap) {
            cap = (len + take + 1) * 2;
            char *bigger = realloc(line, cap);
            if (bigger == NULL) { free(line); return NULL; }
            line = bigger;
        }
        memcpy(line + len, r->buf + r->pos, take);
        len += take;
        r->pos += take;
        line[len] = '\0';
        if (nl) break;
    }
    return line; /* NULL if nothing was read */
}

/* LineSource for a script or sourced file. */
/* set -v: input lines are echoed to stderr as they are read. */
static char *echo_if_verbose(char *line) {
    if (line != NULL && shell_option('v')) {
        fputs(line, stderr);
    }
    return line;
}

static char *file_lines(void *ctx, int continuation) {
    (void)continuation;
    return echo_if_verbose(reader_line((LineReader *)ctx));
}

/* The reader for standard input, allocated when first used: `-c` and
 * scripts never need it, and a 4 KB buffer in static memory would cost
 * every start a page fault. */
static LineReader *stdin_reader(void) {
    static LineReader *r;
    if (r == NULL && (r = calloc(1, sizeof(*r))) == NULL) {
        shell_error("out of memory\n");
        exit(2);
    }
    return r;
}

/* LineSource for standard input. At the interactive prompt it prints
 * PS1 or PS2 and records history; otherwise (commands piped in, -s) it
 * just reads. */
static char *stdin_lines(void *ctx, int continuation) {
    (void)ctx;
    if (!interactive) {
        return echo_if_verbose(reader_line(stdin_reader()));
    }
    if (continuation && shell_interrupted()) {
        return NULL;  /* Ctrl-C cancelled this command: no more lines for it */
    }
    if (!continuation) {
        jobs_report_changes();
    }
    char *line;
    if (lineedit_usable()) {
        char *ps = prompt_string(continuation ? "PS2" : "PS1");
        line = lineedit_read(ps);
        free(ps);
        if (line != NULL && line[0] == '\0') {
            /* Ctrl-C: the line is dropped; a command already begun on
             * earlier lines is cancelled too (main loop) */
            free(line);
            env_set_last_status(130);
            if (continuation) {
                shell_note_sigint();
                return NULL;
            }
            line = strdup("\n");
        }
        line = echo_if_verbose(line);
    } else {
        prompt_print(continuation ? "PS2" : "PS1");
        line = echo_if_verbose(reader_line(stdin_reader()));
    }
    if (line == NULL) {
        return NULL;
    }
    if (!continuation) {
        /* a Ctrl+C that arrived while the prompt sat idle is spent */
        shell_clear_interrupted();
    }
    size_t len = strcspn(line, "\n");
    char *entry = strndup(line, len);
    if (entry != NULL) {
        if (continuation) history_continue(entry);
        else history_begin(entry);
        free(entry);
    }
    return line;
}

/* ---- shell options ($-) ---- */

static char options[128];  /* options['e'] == 1 after set -e */

int shell_option(char c) {
    return (unsigned char)c < sizeof(options) && options[(unsigned char)c];
}

void shell_set_option(char c, int on) {
    if ((unsigned char)c < sizeof(options)) {
        options[(unsigned char)c] = (char)(on != 0);
    }
}

const char *shell_option_string(void) {
    static char buf[32];
    size_t n = 0;
    for (const char *c = "cefimnsCauxv"; *c && n + 1 < sizeof(buf); c++) { /* ash's order */
        if (options[(unsigned char)*c]) buf[n++] = *c;
    }
    buf[n] = '\0';
    return buf;
}

/* ---- running files ---- */

/* After run_lexer(): applies what a syntax error (-1) or Ctrl+C (-2)
 * means here, and returns the status. */
static int finish_run(int r) {
    if (r == -1 && !interactive) {
        trap_run_exit();
        exit(2);
    }
    if (r == -2) {
        if (!interactive) {
            trap_run_exit();
            exit(130);
        }
        env_set_last_status(130);
    }
    control_flow_reset();
    return env_get_last_status();
}


/* Runs the file at `path` command by command, in this shell. Used for
 * scripts, ~/.acshrc and the `source` / `.` builtin. Returns the status
 * of the last command, or -1 if the file cannot be opened. In a
 * non-interactive shell a syntax error or an untrapped Ctrl+C ends the
 * shell, as in ash; at the prompt it only stops this file. */
static int run_file(const char *path, int errors_end_shell);

int run_script_file(const char *path) {
    return run_file(path, 1);
}

/* errors_end_shell: in a non-interactive shell, a syntax error or an
 * untrapped Ctrl+C in this file ends the shell (scripts, `source`); a
 * startup file such as /etc/profile only stops itself. */
static int run_file(const char *path, int errors_end_shell) {
    int raw = open(path, O_RDONLY);
    if (raw < 0) {
        shell_error("can't open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    /* keep the script's own fd out of the 0-9 range a script may
     * redirect, and out of every command it runs */
    LineReader *reader = malloc(sizeof(LineReader));
    if (reader == NULL) {
        close(raw);
        return -1;
    }
    reader->fd = fcntl(raw, F_DUPFD_CLOEXEC, 10);
    if (reader->fd < 0) {
        reader->fd = raw;
    } else {
        close(raw);
    }
    reader->pos = reader->len = 0;

    Lexer *lx = lexer_new("", file_lines, reader, path);
    control_flow_enter_file();
    int r = run_lexer(lx);
    control_flow_leave_file();
    lexer_free(lx);
    close(reader->fd);
    free(reader);
    int returned;
    if (control_flow_take_return(&returned)) {
        env_set_last_status(returned); /* `return N` ended the file */
    }
    if (!errors_end_shell && r < 0) {
        shell_clear_interrupted();
        control_flow_reset();
        env_set_last_status(r == -1 ? 2 : 130);
        return env_get_last_status();
    }
    return finish_run(r);
}

static void run_startup_file(const char *path);

/* A login shell (-l, or argv[0] starting with '-', as login(1) starts
 * it) reads /etc/profile and then ~/.profile, as ash does. On Alpine,
 * /etc/profile itself runs /etc/profile.d/[*].sh. */
static void load_login_profiles(void) {
    if (access("/etc/profile", R_OK) == 0) {
        run_startup_file("/etc/profile");
    }
    const char *home = var_get("HOME");
    if (home != NULL) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/.profile", home);
        if (access(path, R_OK) == 0) {
            run_startup_file(path);
        }
    }
}


/* Files already run at startup, by device and inode: ENV=~/.acshrc,
 * or a ~/.profile that sources ~/.acshrc, must not run it twice. */
static struct { dev_t dev; ino_t ino; } startup_seen[8];
static int nstartup_seen = 0;

static void run_startup_file(const char *path) {
    struct stat st;
    if (path == NULL || *path == '\0' || stat(path, &st) != 0 || access(path, R_OK) != 0) return;
    for (int i = 0; i < nstartup_seen; i++) {
        if (startup_seen[i].dev == st.st_dev && startup_seen[i].ino == st.st_ino) return;
    }
    if (nstartup_seen < 8) {
        startup_seen[nstartup_seen].dev = st.st_dev;
        startup_seen[nstartup_seen++].ino = st.st_ino;
    }
    run_file(path, 0);
}

/* What an interactive shell reads, login or not (so `su`, `docker exec
 * -it` and `lxc exec` shells get the same aliases): /etc/acshrc, then
 * ~/.acshrc, then $ENV (expanded, as POSIX asks). Scripts and -c read
 * none of them. A setuid shell reads none either. */
static void load_interactive_startup(void) {
    if (getuid() != geteuid() || getgid() != getegid()) return;
    run_startup_file("/etc/acshrc");
    const char *home = var_get("HOME");
    if (home != NULL && *home) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/.acshrc", home);
        run_startup_file(path);
    }
    const char *env = var_get("ENV");
    char *marked = (env != NULL && *env) ? lex_mark(env, 1) : NULL;
    char *file = marked != NULL ? expand_string(marked, 0) : NULL;
    run_startup_file(file);
    free(file);
    free(marked);
}

static void usage_error(const char *fmt, const char *arg) {
    shell_error_prefix();
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\nusage: acsh [-eilnsux] [-c COMMAND [NAME [ARG...]] | -s [ARG...] | FILE [ARG...]]\n");
    exit(2);
}

/* Invocation, as in ash:
 *   acsh [options] -c COMMAND [NAME [ARG...]]   run COMMAND; $0 = NAME
 *   acsh [options] FILE [ARG...]                run FILE; $0 = FILE
 *   acsh [options] [-s] [ARG...]                read commands from stdin
 * Options: -c -s -i (interactive), and the set options -e -u -x -f -n
 * -v -a -C. Standard input is interactive (prompt, history, default
 * aliases, ~/.acshrc) when -i is given, or when no -c or FILE is given
 * and stdin and stderr are terminals. */
int main(int argc, char *argv[]) {
    stack_init(&argc);
    vars_init();
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", (int)getppid());
        var_set("PPID", buf);
        /* $PWD: keep an inherited value that still names the current
         * directory (it may go through symlinks), else the real path */
        char cwd[4096];
        struct stat a, b;
        const char *pwd = var_get("PWD");
        if (!(pwd != NULL && pwd[0] == '/' && stat(pwd, &a) == 0 && stat(".", &b) == 0 &&
              a.st_dev == b.st_dev && a.st_ino == b.st_ino) && getcwd(cwd, sizeof(cwd)) != NULL) {
            var_set("PWD", cwd);
        }
        var_add_flags("PWD", V_EXPORT);
        /* the defaults ash sets (PS1 stays acsh's own prompt for now) */
        var_set("IFS", " \t\n"); /* never inherited (POSIX) */
        if (var_get("PS2") == NULL) var_set("PS2", "> ");
        if (var_get("PS4") == NULL) var_set("PS4", "+ ");
        var_set("OPTIND", "1");
        shell_set_option('B', 1); /* brace expansion, as in bash; set +B turns it off */
        if (var_get("PATH") == NULL) {
            var_set("PATH", "/sbin:/usr/sbin:/bin:/usr/bin"); /* ash's default, exported */
            var_add_flags("PATH", V_EXPORT);
        }
        char host[256];
        if (var_get("HOSTNAME") == NULL && gethostname(host, sizeof(host)) == 0) {
            host[sizeof(host) - 1] = '\0';
            var_set("HOSTNAME", host);
        }
    }
    int i = 1;
    int force_interactive = 0;
    int login = (argv[0] != NULL && argv[0][0] == '-');
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if ((a[0] != '-' && a[0] != '+') || a[1] == '\0') {
            break;
        }
        for (const char *c = a + 1; *c; c++) {
            if (*c == 'i') {
                force_interactive = (a[0] == '-');
            } else if (*c == 'l') {
                login = (a[0] == '-');
            } else if (strchr("cs" "aefnuvxC", *c) != NULL) {
                shell_set_option(*c, a[0] == '-');
            } else {
                char opt[3] = { a[0], *c, '\0' };
                usage_error("illegal option %s", opt);
            }
        }
    }

    const char *command = NULL;
    const char *script = NULL;
    const char *name = argv[0];
    if (shell_option('c')) {
        if (i >= argc) {
            usage_error("%s requires an argument", "-c");
        }
        command = argv[i++];
        if (i < argc) {
            name = argv[i++];
        }
    } else if (!shell_option('s') && i < argc) {
        script = argv[i++];
        name = script;
    } else {
        shell_set_option('s', 1);
    }
    env_init(name, argc - i, argv + i);

    interactive = force_interactive ||
                  (command == NULL && script == NULL && isatty(STDIN_FILENO) && isatty(STDERR_FILENO));
    shell_set_option('i', interactive);
    signals_init();
    if (interactive) {
        job_control_init();
    }
    if (interactive) {
        alias_load_defaults();  /* first, so profiles and ~/.acshrc can override them */
        /* PS1 is set only in interactive shells (as bash does), so the
         * common test [ "$PS1" ] in a profile tells them from scripts */
        if (var_get("PS1") == NULL) var_set("PS1", "acsh:\\w\\g\\$ ");
    }
    if (login) {
        load_login_profiles();
    }
    if (interactive) {
        load_interactive_startup();
    }

    if (command != NULL) {
        Lexer *lx = lexer_new(command, NULL, NULL, "-c");
        finish_run(run_lexer(lx));
        lexer_free(lx);
        trap_run_exit();
        return env_get_last_status();
    }
    if (script != NULL) {
        if (run_script_file(script) < 0) {
            return 2; /* as ash does for a script it cannot open */
        }
        trap_run_exit();
        return env_get_last_status();
    }

    /* standard input */
    if (interactive) {
        history_load();
    }
    Lexer *lx = lexer_new("", stdin_lines, NULL, interactive ? NULL : "stdin");
    for (;;) {
        int r = run_lexer(lx);
        if (r == 0) {
            break; /* Ctrl+D / end of input */
        }
        if ((r == -2 || shell_interrupted()) && interactive) {
            /* Ctrl+C stopped a command, or cancelled one being typed:
             * drop the rest of what was typed */
            shell_clear_interrupted();
            lex_discard(lx);
            env_set_last_status(130);
            continue;
        }
        finish_run(r);
    }
    lexer_free(lx);
    if (interactive) {
        fputc('\n', stderr);
    }
    trap_run_exit();
    return env_get_last_status();
}
