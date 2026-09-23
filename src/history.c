#define _POSIX_C_SOURCE 200809L  /* strdup under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "acsh.h"

static char  history_buf[ACSH_HISTORY_SIZE][ACSH_MAX_LINE];
static int   history_count = 0;   /* total entries added this session+loaded */
static int   history_start = 0;   /* index of the oldest entry (ring buffer) */

/* Builds the full path to ~/.acsh_history into `out`. Returns 0 on
 * success, -1 if HOME isn't set (history is simply skipped then). */
static int history_file_path(char *out, size_t outsize) {
    const char *home = getenv("HOME");
    if (home == NULL) {
        return -1;
    }
    snprintf(out, outsize, "%s/.acsh_history", home);
    return 0;
}

/* Internal: pushes one line into the ring buffer without touching the
 * file on disk (used both for loading existing history and for new
 * entries added during this session). */
static void history_push(const char *line) {
    int idx = (history_start + (history_count % ACSH_HISTORY_SIZE)) % ACSH_HISTORY_SIZE;

    if (history_count >= ACSH_HISTORY_SIZE) {
        /* buffer is full: overwrite the oldest slot and advance start */
        idx = history_start;
        history_start = (history_start + 1) % ACSH_HISTORY_SIZE;
    }

    strncpy(history_buf[idx], line, ACSH_MAX_LINE - 1);
    history_buf[idx][ACSH_MAX_LINE - 1] = '\0';

    if (history_count < ACSH_HISTORY_SIZE) {
        history_count++;
    }
}

void history_load(void) {
    char path[512];
    if (history_file_path(path, sizeof(path)) != 0) {
        return;
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return; /* no history file yet -- fine, first run */
    }

    char line[ACSH_MAX_LINE];
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] != '\0') {
            history_push(line);
        }
    }
    fclose(f);
}

void history_add(const char *line) {
    if (line[0] == '\0') {
        return; /* skip blank lines */
    }

    /* skip if identical to the immediately preceding entry
     * (HISTCONTROL=ignoredups-style behaviour) */
    if (history_count > 0) {
        int last_idx = (history_start + history_count - 1) % ACSH_HISTORY_SIZE;
        if (strcmp(history_buf[last_idx], line) == 0) {
            return;
        }
    }

    history_push(line);
}

void history_save_append(void) {
    char path[512];
    if (history_file_path(path, sizeof(path)) != 0) {
        return;
    }
    if (history_count == 0) {
        return;
    }

    FILE *f = fopen(path, "a");
    if (f == NULL) {
        return; /* silently skip -- history persistence is best-effort */
    }

    int last_idx = (history_start + history_count - 1) % ACSH_HISTORY_SIZE;
    fprintf(f, "%s\n", history_buf[last_idx]);
    fclose(f);
}

void history_print_all(void) {
    for (int i = 0; i < history_count; i++) {
        int idx = (history_start + i) % ACSH_HISTORY_SIZE;
        printf("%5d  %s\n", i + 1, history_buf[idx]);
    }
}
