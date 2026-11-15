#define _POSIX_C_SOURCE 200809L  /* strdup under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "acsh.h"

static char *history_buf[ACSH_HISTORY_SIZE];  /* heap strings */
static int   history_count = 0;   /* total entries added this session+loaded */
static int   history_start = 0;   /* index of the oldest entry (ring buffer) */

/* Builds the full path to ~/.acsh_history into `out`. Returns 0 on
 * success, -1 if HOME isn't set (history is simply skipped then). */
static int history_file_path(char *out, size_t outsize) {
    const char *home = var_get("HOME");
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
    char *copy = strdup(line);
    if (copy == NULL) {
        return;
    }
    int idx = (history_start + (history_count % ACSH_HISTORY_SIZE)) % ACSH_HISTORY_SIZE;

    if (history_count >= ACSH_HISTORY_SIZE) {
        /* buffer is full: overwrite the oldest slot and advance start */
        idx = history_start;
        history_start = (history_start + 1) % ACSH_HISTORY_SIZE;
    }

    free(history_buf[idx]); /* the oldest entry, once the ring is full */
    history_buf[idx] = copy;

    if (history_count < ACSH_HISTORY_SIZE) {
        history_count++;
    }
}

/* Opens the history file for appending, creating it readable only by
 * its owner: typed commands can contain passwords and tokens. */
static FILE *open_history_for_append(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        return NULL;
    }
    FILE *f = fdopen(fd, "a");
    if (f == NULL) {
        close(fd);
    }
    return f;
}

/* Replaces the history file with exactly the entries held in memory.
 * Written to a temporary file first and renamed over the old one, so a
 * crash mid-write never leaves a half-written history. */
static void history_rewrite_file(const char *path) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return;
    }
    FILE *f = fdopen(fd, "w");
    if (f == NULL) {
        close(fd);
        unlink(tmp);
        return;
    }
    for (int i = 0; i < history_count; i++) {
        fprintf(f, "%s\n", history_buf[(history_start + i) % ACSH_HISTORY_SIZE]);
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
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

    char *line = NULL;
    size_t cap = 0;
    int file_entries = 0;
    while (getline(&line, &cap, f) >= 0) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] != '\0') {
            history_push(line);
            file_entries++;
        }
    }
    free(line);
    fclose(f);

    /* Only the last ACSH_HISTORY_SIZE entries are ever loaded, so a
     * longer file is trimmed to those, or it would grow forever. */
    if (file_entries > ACSH_HISTORY_SIZE) {
        history_rewrite_file(path);
    }
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

    FILE *f = open_history_for_append(path);
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
