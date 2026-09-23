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

/* Runs exactly one line of shell input: alias expansion, parse,
 * execute. Shared by the interactive REPL and by the ~/.acshrc loader
 * so both go through the identical code path. `record_history`
 * controls whether this line should be added to command history --
 * we don't want rc-file lines cluttering the user's history. */
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

    /* alias expansion happens on the raw line, BEFORE parsing, since
     * an alias's expansion text is itself shell syntax (e.g. `alias
     * ll='ls -l'` must inject "ls -l" as real words, not one opaque
     * argument) */
    char *expanded = alias_expand_line(line);
    char *line_to_parse = (expanded != NULL) ? expanded : line;

    Pipeline pl;
    memset(&pl, 0, sizeof(pl));

    if (parse_line(line_to_parse, &pl) == 0 && pl.num_cmds > 0) {
        execute_pipeline(&pl);
    }
    /* else: parser already printed an error message */

    free(expanded); /* safe no-op if expanded is NULL */
}

/* Runs ~/.acshrc at startup if it exists, one line at a time, the same
 * way an interactive user would type them (so `alias`, `export`, etc.
 * in the rc file behave exactly like typing them at the prompt). This
 * directly answers the community complaint that switching shells on
 * Alpine leaves you with an empty, unhelpful rc file. */
static void load_rc_file(void) {
    const char *home = getenv("HOME");
    if (home == NULL) {
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/.acshrc", home);

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return; /* no rc file -- fine, defaults already loaded */
    }

    char line[ACSH_MAX_LINE];
    while (fgets(line, sizeof(line), f) != NULL) {
        run_line(line, /*record_history=*/0);
    }
    fclose(f);
}

int main(void) {
    char line[ACSH_MAX_LINE];

    signals_init();

    alias_load_defaults();  /* ll, la, .., etc. -- before rc file so it can override */
    load_rc_file();
    history_load();

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
