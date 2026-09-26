#define _POSIX_C_SOURCE 200809L  /* getcwd, etc. under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "acsh.h"

/* Prints the prompt. Kept in its own function so we can later make it
 * configurable (current dir, user@host, etc.) without touching main(). */
static void print_prompt(void) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("acsh:%s$ ", cwd);
    } else {
        printf("acsh$ ");
    }
    fflush(stdout);
}

/* The FILE* run_line() should read FROM when a multi-line construct
 * (currently just if/fi) needs more input than fits on the line it
 * was first called with. Interactive mode points this at stdin;
 * run_script_file() points it at the script being read. Kept as a
 * single static since acsh is single-threaded and run_line() is never
 * reentered while a multi-line construct is still being collected. */
static FILE *current_input_source = NULL;

/* Adapts fgets(current_input_source) to the char *(*)(char*, int)
 * signature run_if_statement() expects for reading additional lines. */
static char *read_more_line_from_current_source(char *buf, int size) {
    if (current_input_source == NULL) {
        return NULL;
    }
    return fgets(buf, size, current_input_source);
}

/* Runs exactly one line of shell input: splits it into &&/||/;
 * connected raw segments, then hands off to execute_chain() (which
 * alias-expands, parses, and $VAR/$?-expands each segment one at a
 * time, immediately before running it -- see execute_chain()'s
 * comment in executor.c for why that ordering matters for $?).
 * Shared by the interactive REPL and by the ~/.acshrc loader so both
 * go through the identical code path. `record_history` controls
 * whether this line should be added to command history -- we don't
 * want rc-file lines cluttering the user's history. */
static void run_line(char *line, int record_history) {
    /* strip trailing newline, if any (fgets keeps it) */
    line[strcspn(line, "\n")] = '\0';

    if (line[0] == '\0') {
        return;
    }

    if (record_history) {
        history_add(line);
        history_save_append();
    }

    /* if/then/.../fi is handled as its own construct, separate from
     * the &&/||/; Chain machinery, since it can span multiple lines
     * and has its own keyword-based grammar. Detected by first word
     * only, so this never fires on a line that merely CONTAINS "if"
     * elsewhere (e.g. `echo "if you can"` is not a syntax error). */
    if (is_if_statement(line)) {
        int status = run_if_statement(line, read_more_line_from_current_source);
        env_set_last_status(status);
        return;
    }
    if (is_while_statement(line)) {
        int status = run_loop_statement(line, /*is_until=*/0, read_more_line_from_current_source);
        env_set_last_status(status);
        return;
    }
    if (is_until_statement(line)) {
        int status = run_loop_statement(line, /*is_until=*/1, read_more_line_from_current_source);
        env_set_last_status(status);
        return;
    }

    Chain ch;
    if (parse_chain(line, &ch) != 0) {
        return; /* parse_chain already printed an error */
    }
    if (ch.num_segments == 0) {
        return;
    }

    execute_chain(&ch);
}

/* Runs every line of the file at `path` through run_line(), the same
 * way an interactive user would type them one by one. Used by
 * load_rc_file() below and by the `source` / `.` builtin
 * (builtins.c), so a script sourced manually behaves identically to
 * ~/.acshrc at startup. Lines run this way are never added to
 * history -- only lines actually typed at the interactive prompt are. */
int run_script_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "acsh: %s: no such file or directory\n", path);
        return -1;
    }

    FILE *saved_source = current_input_source;
    current_input_source = f;

    char line[ACSH_MAX_LINE];
    while (fgets(line, sizeof(line), f) != NULL) {
        run_line(line, /*record_history=*/0);
    }

    current_input_source = saved_source;
    fclose(f);
    return 0;
}

/* Runs ~/.acshrc at startup if it exists. This directly answers the
 * community complaint that switching shells on Alpine leaves you with
 * an empty, unhelpful rc file. */
static void load_rc_file(void) {
    const char *home = getenv("HOME");
    if (home == NULL) {
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/.acshrc", home);

    /* silently skip if missing -- defaults are already loaded and an
     * absent rc file is a completely normal, expected case, not an
     * error worth printing */
    FILE *probe = fopen(path, "r");
    if (probe == NULL) {
        return;
    }
    fclose(probe);

    run_script_file(path);
}

int main(void) {
    char line[ACSH_MAX_LINE];

    signals_init();

    alias_load_defaults();  /* ll, la, .., etc. -- before rc file so it can override */
    load_rc_file();
    history_load();

    current_input_source = stdin;

    while (1) {
        print_prompt();

        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* Ctrl+D / EOF on stdin -> exit cleanly like a real shell */
            printf("\n");
            break;
        }

        run_line(line, /*record_history=*/1);
    }

    return 0;
}
