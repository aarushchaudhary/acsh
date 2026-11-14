#define _POSIX_C_SOURCE 200809L  /* getcwd, getline, fdopen under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
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

/* PS1. Kept in its own function so it can become configurable later. */
static void print_prompt(void) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("acsh:%s$ ", cwd);
    } else {
        printf("acsh$ ");
    }
    fflush(stdout);
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
static char *file_lines(void *ctx, int continuation) {
    (void)continuation;
    return reader_line((LineReader *)ctx);
}

static LineReader stdin_reader = { 0, { 0 }, 0, 0 };

/* LineSource for the prompt: prints PS1 or PS2 and records history. */
static char *prompt_lines(void *ctx, int continuation) {
    (void)ctx;
    if (!continuation) {
        jobs_report_changes();
        print_prompt();
    } else {
        printf("> ");
        fflush(stdout);
    }
    char *line = reader_line(&stdin_reader);
    if (line == NULL) {
        return NULL;
    }
    if (!continuation) {
        /* a Ctrl+C that arrived while the prompt sat idle is spent */
        shell_clear_interrupted();
    }
    size_t len = strcspn(line, "\n");
    if (len > 0) {
        char *entry = strndup(line, len);
        if (entry != NULL) {
            history_add(entry);
            history_save_append();
            free(entry);
        }
    }
    return line;
}

/* Runs the script at `path` command by command. Used for scripts, for
 * ~/.acshrc and by the `source` / `.` builtin. Returns the status of
 * the last command, or -1 if the file cannot be opened. In a script a
 * syntax error or an untrapped Ctrl+C ends the shell, as in ash; at the
 * prompt (rc file, source) it only stops this file. */
int run_script_file(const char *path) {
    int raw = open(path, O_RDONLY);
    if (raw < 0) {
        fprintf(stderr, "acsh: %s: no such file or directory\n", path);
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
    int r = run_lexer(lx);
    lexer_free(lx);
    close(reader->fd);
    free(reader);

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

/* Runs ~/.acshrc at startup if it exists, so a fresh install is useful
 * without the ENV workaround ash needs. */
static void load_rc_file(void) {
    const char *home = getenv("HOME");
    if (home == NULL) {
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/.acshrc", home);
    if (access(path, R_OK) == 0) {
        run_script_file(path);
    }
}

/* `acsh SCRIPTFILE`: runs the file without a prompt or history and
 * exits with its last command's status. No default aliases, ~/.acshrc
 * or history are loaded: a script must behave the same for everyone
 * (a default alias such as grep='grep --color=auto' would silently
 * change what every script's grep does). */
static int run_as_script(const char *path) {
    signals_init();
    int status = run_script_file(path);
    if (status < 0) {
        return 127;
    }
    trap_run_exit();
    return env_get_last_status();
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        return run_as_script(argv[1]);
    }

    signals_init();
    interactive = 1;
    alias_load_defaults();  /* ll, la, .., etc. -- before the rc file so it can override */
    load_rc_file();
    history_load();

    Lexer *lx = lexer_new("", prompt_lines, NULL, NULL);
    for (;;) {
        int r = run_lexer(lx);
        if (r == 0) {
            break; /* Ctrl+D / end of input */
        }
        if (r == -2) {
            /* Ctrl+C stopped a command: drop the rest of what was typed */
            env_set_last_status(130);
            shell_clear_interrupted();
            lex_discard_input(lx);
        }
        control_flow_reset();
    }
    lexer_free(lx);
    printf("\n");
    trap_run_exit();
    return env_get_last_status();
}
