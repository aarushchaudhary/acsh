#define _DEFAULT_SOURCE          /* flock() */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include "acsh.h"

/* Command history.
 *
 * One entry per command, a command typed over several lines included.
 * Kept in memory (the last $HISTSIZE) and in $HISTFILE (default
 * ~/.acsh_history, mode 0600; HISTFILE= keeps no file).
 *
 * The file is one entry per line; a line ending in a backslash goes on
 * with the next one (as zsh writes multi-line entries), so a command
 * with newlines in it cannot be split into several entries. New entries
 * are appended under flock(), so two shells writing at once leave a
 * clean file; the file is trimmed (rewritten through a temporary file
 * and a rename, under the same lock) when it grows past $HISTSIZE.
 *
 * $HISTCONTROL: ignorespace (a line starting with a blank is not kept,
 * the convention Midnight Commander relies on), ignoredups, ignoreboth,
 * erasedups. Unset, it is ignoreboth. $HISTIGNORE: colon-separated
 * patterns for whole lines never to keep; & is the previous entry. */

#define DEFAULT_HISTSIZE 1000

static char **items;       /* oldest first */
static int    count, cap;
static int    base = 1;     /* the number of items[0], for `history` and fc */
static int    saved;        /* items[0, saved) are already in the file */
static int    open_entry;   /* the newest entry is still being typed (more lines may come) */
static off_t  file_read_to; /* history -n: the file's bytes up to here were read */

static int histsize(void) {
    const char *v = var_get("HISTSIZE");
    if (v == NULL || *v == '\0') return DEFAULT_HISTSIZE;
    long n = strtol(v, NULL, 10);
    return n < 0 ? 0 : n > 1000000 ? 1000000 : (int)n;
}

/* The history file, or NULL for none. */
static const char *history_path(char *buf, size_t size) {
    const char *f = var_get("HISTFILE");
    if (f != NULL) return *f ? f : NULL;
    const char *home = var_get("HOME");
    if (home == NULL || *home == '\0') return NULL;
    snprintf(buf, size, "%s/.acsh_history", home);
    return buf;
}

static void drop_oldest(int n) {
    if (n <= 0) return;
    if (n > count) n = count;
    for (int i = 0; i < n; i++) free(items[i]);
    memmove(items, items + n, (size_t)(count - n) * sizeof(char *));
    count -= n;
    base += n;
    saved = saved > n ? saved - n : 0;
}

static void push(char *entry) {
    if (count == cap) {
        int ncap = cap ? cap * 2 : 64;
        char **bigger = realloc(items, (size_t)ncap * sizeof(char *));
        if (bigger == NULL) { free(entry); return; }
        items = bigger;
        cap = ncap;
    }
    items[count++] = entry;
    drop_oldest(count - histsize());
}

static void remove_at(int i) {
    free(items[i]);
    memmove(items + i, items + i + 1, (size_t)(count - i - 1) * sizeof(char *));
    count--;
    if (i < saved) saved--;
}

/* ---- the file ---- */

/* Opens path for appending and locks it; after a wait for the lock, a
 * file another shell replaced (trimmed) meanwhile is opened again. */
static int open_locked(const char *path, int flags) {
    for (int tries = 0; tries < 5; tries++) {
        int fd = open(path, flags | O_CREAT, 0600);
        if (fd < 0) return -1;
        if (flock(fd, LOCK_EX) != 0) { close(fd); return -1; }
        struct stat a, b;
        if (fstat(fd, &a) == 0 && stat(path, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
            return fd;
        }
        close(fd);  /* renamed away while we waited: take the new file */
    }
    return -1;
}

/* Writes an entry in the file format: newlines become backslash-newline. */
static void write_entry(FILE *f, const char *e) {
    for (const char *s = e; *s; s++) {
        if (*s == '\n') fputs("\\\n", f);
        else fputc(*s, f);
    }
    fputc('\n', f);
}

/* Reads entries from f; an entry whose line ends in a backslash goes on
 * with the next line. Each one goes to add(). */
static void read_entries(FILE *f, void (*add)(char *)) {
    char *line = NULL, *entry = NULL;
    size_t lcap = 0, elen = 0;
    ssize_t n;
    while ((n = getline(&line, &lcap, f)) >= 0) {
        if (n > 0 && line[n - 1] == '\n') line[--n] = '\0';
        int more = n > 0 && line[n - 1] == '\\';
        if (more) line[--n] = '\0';
        char *grown = realloc(entry, elen + (size_t)n + 2);
        if (grown == NULL) break;
        entry = grown;
        memcpy(entry + elen, line, (size_t)n);
        elen += (size_t)n;
        if (more) {
            entry[elen++] = '\n';
            continue;
        }
        entry[elen] = '\0';
        if (elen > 0) add(entry);
        else free(entry);
        entry = NULL;
        elen = 0;
    }
    if (entry != NULL && elen > 0) {   /* a last line cut short: keep what is there */
        entry[elen] = '\0';
        add(entry);
    } else {
        free(entry);
    }
    free(line);
}

static void add_loaded(char *e) {
    push(e);
    saved = count;
}

/* Rewrites the file with the entries in memory (under the lock held on fd). */
static void rewrite_file(const char *path, int locked_fd) {
    char tmp[4200];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "w");
    if (f == NULL) { close(fd); unlink(tmp); return; }
    for (int i = 0; i < count; i++) write_entry(f, items[i]);
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return;
    }
    saved = count;
    struct stat st;
    file_read_to = (stat(path, &st) == 0) ? st.st_size : 0;
    (void)locked_fd;
}

/* Keeps only the newest $HISTSIZE entries in the file. Read and
 * rewritten under the lock, so what other shells appended stays. */
static char **trim_list;
static int    trim_n, trim_cap;

static void add_trim(char *e) {
    if (trim_n == trim_cap) {
        int ncap = trim_cap ? trim_cap * 2 : 256;
        char **bigger = realloc(trim_list, (size_t)ncap * sizeof(char *));
        if (bigger == NULL) { free(e); return; }
        trim_list = bigger;
        trim_cap = ncap;
    }
    trim_list[trim_n++] = e;
}

static void trim_file(void) {
    char buf[4096];
    const char *path = history_path(buf, sizeof(buf));
    if (path == NULL) return;
    int fd = open_locked(path, O_RDONLY);
    if (fd < 0) return;
    FILE *f = fdopen(dup(fd), "r");
    if (f != NULL) {
        read_entries(f, add_trim);
        fclose(f);
    }
    int keep = histsize();
    if (trim_n > keep) {
        char tmp[4200];
        snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
        int tfd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
        FILE *out = tfd >= 0 ? fdopen(tfd, "w") : NULL;
        if (out != NULL) {
            for (int i = trim_n - keep; i < trim_n; i++) write_entry(out, trim_list[i]);
            if (fclose(out) != 0 || rename(tmp, path) != 0) unlink(tmp);
        } else if (tfd >= 0) {
            close(tfd);
            unlink(tmp);
        }
    }
    for (int i = 0; i < trim_n; i++) free(trim_list[i]);
    free(trim_list);
    trim_list = NULL;
    trim_n = trim_cap = 0;
    close(fd);
}

/* Reads the whole file into the list (history -r, and at startup). */
static void read_file(void) {
    char buf[4096];
    const char *path = history_path(buf, sizeof(buf));
    if (path == NULL) return;
    int fd = open_locked(path, O_RDONLY);
    if (fd < 0) return;
    FILE *f = fdopen(dup(fd), "r");
    if (f != NULL) {
        read_entries(f, add_loaded);
        file_read_to = ftello(f);
        fclose(f);
    }
    close(fd);
}

static void flush_file(void) {
    if (saved >= count || (open_entry && saved == count - 1)) return;
    char buf[4096];
    const char *path = history_path(buf, sizeof(buf));
    if (path == NULL) { saved = count; return; }
    int fd = open_locked(path, O_WRONLY | O_APPEND);
    if (fd < 0) return;  /* best effort: no history file is not an error */
    off_t before = lseek(fd, 0, SEEK_END);
    FILE *f = fdopen(dup(fd), "a");
    if (f != NULL) {
        int upto = open_entry ? count - 1 : count;  /* an entry still being typed waits */
        for (int i = saved; i < upto; i++) write_entry(f, items[i]);
        fclose(f);
        saved = upto;
        if (before == file_read_to) file_read_to = lseek(fd, 0, SEEK_END);
    }
    close(fd);
}

/* ---- adding entries ---- */

static int control_has(const char *word) {
    const char *v = var_get("HISTCONTROL");
    if (v == NULL) v = "ignoreboth";
    size_t n = strlen(word);
    for (const char *p = v; *p;) {
        const char *end = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if ((len == n && strncmp(p, word, n) == 0) ||
            (len == 10 && strncmp(p, "ignoreboth", 10) == 0 &&
             (strcmp(word, "ignorespace") == 0 || strcmp(word, "ignoredups") == 0))) {
            return 1;
        }
        p += len + (end != NULL);
    }
    return 0;
}

static int ignored_by_pattern(const char *line) {
    const char *v = var_get("HISTIGNORE");
    if (v == NULL || *v == '\0') return 0;
    char *copy = strdup(v);
    int hit = 0;
    for (char *p = copy, *next; p != NULL && !hit; p = next) {
        next = strchr(p, ':');
        if (next != NULL) *next++ = '\0';
        if (strcmp(p, "&") == 0) hit = count > 0 && strcmp(items[count - 1], line) == 0;
        else if (*p) hit = fnmatch(p, line, 0) == 0;
    }
    free(copy);
    return hit;
}

void history_begin(const char *line) {
    open_entry = 0;
    flush_file();  /* the previous command is complete now */
    if (*line == '\0' || histsize() == 0) return;
    if ((*line == ' ' || *line == '\t') && control_has("ignorespace")) return;
    if (ignored_by_pattern(line)) return;
    if (count > 0 && strcmp(items[count - 1], line) == 0 && control_has("ignoredups")) return;
    if (control_has("erasedups")) {
        for (int i = count - 1; i >= 0; i--) {
            if (strcmp(items[i], line) == 0) remove_at(i);
        }
    }
    char *copy = strdup(line);
    if (copy == NULL) return;
    push(copy);
    open_entry = 1;
}

void history_continue(const char *line) {
    if (!open_entry || count == 0) return;
    char *old = items[count - 1];
    size_t a = strlen(old), b = strlen(line);
    char *joined = realloc(old, a + b + 2);
    if (joined == NULL) return;
    joined[a] = '\n';
    memcpy(joined + a + 1, line, b + 1);
    items[count - 1] = joined;
}

void history_finish(void) {
    open_entry = 0;
    flush_file();
}

static void history_at_exit(void) {
    history_finish();
    trim_file();
}

void history_load(void) {
    trim_file();
    read_file();
    atexit(history_at_exit);
}

int history_number(void) {
    return base + count;
}

int history_length(void) {
    return count;
}

const char *history_get(int i) {
    return (i >= 0 && i < count) ? items[i] : NULL;
}

/* ---- history and fc ---- */

static int usage(const char *who, const char *msg) {
    shell_error("%s: %s\n", who, msg);
    return 2;
}

/* history [N] | -c | -d N | -a | -n | -r | -w: lists on stdout (so
 * `history 2>/dev/null` still prints), or works on the list and file. */
int history_builtin(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] != '\0') {
        const char *o = argv[1] + 1;
        if (strcmp(o, "c") == 0) {
            drop_oldest(count);
            saved = 0;
            open_entry = 0;
            return 0;
        }
        if (strcmp(o, "d") == 0) {
            if (argc < 3) return usage("history", "-d: needs an entry number");
            char *end;
            long n = strtol(argv[2], &end, 10);
            if (n < 0) n = base + count + n;
            if (*end != '\0' || n < base || n >= base + count) {
                shell_error("history: %s: no such entry\n", argv[2]);
                return 1;
            }
            remove_at((int)(n - base));
            return 0;
        }
        open_entry = 0;   /* the history command itself is complete */
        if (strcmp(o, "a") == 0) {
            flush_file();
            return 0;
        }
        if (strcmp(o, "w") == 0 || strcmp(o, "n") == 0 || strcmp(o, "r") == 0) {
            char buf[4096];
            const char *path = history_path(buf, sizeof(buf));
            if (path == NULL) return 0;
            if (*o == 'r') {
                read_file();
                return 0;
            }
            int fd = open_locked(path, *o == 'w' ? O_WRONLY : O_RDONLY);
            if (fd < 0) {
                shell_error("history: %s: %s\n", path, strerror(errno));
                return 1;
            }
            if (*o == 'w') {
                rewrite_file(path, fd);
            } else {          /* -n: what other shells appended since we last read */
                FILE *f = fdopen(dup(fd), "r");
                if (f != NULL) {
                    fseeko(f, file_read_to, SEEK_SET);
                    read_entries(f, add_loaded);
                    file_read_to = ftello(f);
                    fclose(f);
                }
            }
            close(fd);
            return 0;
        }
        return usage("history", "usage: history [N] | -c | -d N | -a | -n | -r | -w");
    }
    int from = 0;
    if (argc > 1) {
        char *end;
        long n = strtol(argv[1], &end, 10);
        if (*end != '\0' || n < 0) return usage("history", "usage: history [N] | -c | -d N | -a | -n | -r | -w");
        if (n < count) from = count - (int)n;
    }
    for (int i = from; i < count; i++) {
        printf("%5d  ", base + i);
        for (const char *s = items[i]; *s; s++) {
            putchar(*s);
            if (*s == '\n') fputs("       ", stdout);  /* line up continuation lines */
        }
        putchar('\n');
    }
    return 0;
}

/* An fc operand: a number (negative counts back from the newest), or
 * the newest entry starting with that text. The fc command itself, the
 * newest entry, is not counted. Returns an index or -1. */
static int fc_find(const char *spec, int last) {
    char *end;
    long n = strtol(spec, &end, 10);
    if (*spec && *end == '\0') {
        long i = n < 0 ? last + 1 + n : n - base;
        if (i < 0) i = 0;
        if (i > last) i = last;
        return (int)i;
    }
    for (int i = last; i >= 0; i--) {
        if (strncmp(items[i], spec, strlen(spec)) == 0) return i;
    }
    return -1;
}

/* fc -l [-nr] [FIRST [LAST]]   list (default: the last 16)
 * fc -s [OLD=NEW] [FIRST]      run an entry again, with OLD replaced
 * fc [-e EDITOR] [-r] [FIRST [LAST]]  edit entries, then run them
 * The editor is -e, else $FCEDIT, else $EDITOR, else vi. */
int fc_builtin(int argc, char **argv) {
    int list = 0, again = 0, nonum = 0, reverse = 0, i = 1;
    const char *editor = NULL;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0' && !(argv[i][1] >= '0' && argv[i][1] <= '9'); i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (const char *c = argv[i] + 1; *c; c++) {
            if (*c == 'l') list = 1;
            else if (*c == 's') again = 1;
            else if (*c == 'n') nonum = 1;
            else if (*c == 'r') reverse = 1;
            else if (*c == 'e') {
                if (c[1] != '\0') { editor = c + 1; break; }
                if (++i >= argc) return usage("fc", "-e: needs an editor");
                editor = argv[i];
                break;
            } else {
                return usage("fc", "usage: fc [-e EDITOR] [-lnr] [FIRST [LAST]] | fc -s [OLD=NEW] [FIRST]");
            }
        }
    }
    /* the fc line itself is the newest entry: it is never an operand,
     * and with -s or an edit it is replaced by what runs */
    int last = count - 1;
    if (last >= 0 && strncmp(items[last], "fc", 2) == 0 && (items[last][2] == '\0' || items[last][2] == ' ')) last--;
    if (last < 0) {
        shell_error("fc: history is empty\n");
        return 1;
    }
    if (again) {
        const char *old = NULL, *rep = NULL;
        char *pair = NULL;
        if (i < argc && strchr(argv[i], '=') != NULL) {
            pair = strdup(argv[i++]);
            char *eq = strchr(pair, '=');
            *eq = '\0';
            old = pair;
            rep = eq + 1;
        }
        int at = i < argc ? fc_find(argv[i], last) : last;
        if (at < 0) {
            shell_error("fc: %s: no such command in history\n", argv[i]);
            free(pair);
            return 1;
        }
        const char *cmd = items[at];
        char *run;
        const char *hit = (old != NULL && *old) ? strstr(cmd, old) : NULL;
        if (hit != NULL) {
            size_t a = (size_t)(hit - cmd), ol = strlen(old), rl = strlen(rep);
            run = malloc(strlen(cmd) - ol + rl + 1);
            memcpy(run, cmd, a);
            memcpy(run + a, rep, rl);
            strcpy(run + a + rl, hit + ol);
        } else {
            run = strdup(cmd);
        }
        free(pair);
        if (last == count - 2) remove_at(count - 1);  /* drop the fc line */
        printf("%s\n", run);
        fflush(stdout);
        open_entry = 0;
        history_begin(run);
        open_entry = 0;
        int status = run_string(run);
        free(run);
        return status;
    }
    int first, end;
    if (i < argc) {
        first = fc_find(argv[i], last);
        end = (i + 1 < argc) ? fc_find(argv[i + 1], last) : (list ? last : first);
    } else {
        first = list ? (last - 15 < 0 ? 0 : last - 15) : last;
        end = last;
    }
    if (first < 0 || end < 0) {
        shell_error("fc: no such command in history\n");
        return 1;
    }
    if (first > end) {
        int t = first; first = end; end = t;
        reverse = !reverse;
    }
    if (list) {
        for (int k = 0; k <= end - first; k++) {
            int at = reverse ? end - k : first + k;
            if (!nonum) printf("%d\t", base + at);
            printf("%s\n", items[at]);
        }
        return 0;
    }
    /* edit: write the entries to a file, run the editor on it, then run
     * what the file holds */
    char tmp[] = "/tmp/acsh-fc-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) {
        shell_error("fc: %s\n", strerror(errno));
        return 1;
    }
    FILE *f = fdopen(fd, "w");
    for (int k = 0; k <= end - first; k++) fprintf(f, "%s\n", items[reverse ? end - k : first + k]);
    fclose(f);
    if (editor == NULL) editor = var_get("FCEDIT");
    if (editor == NULL || *editor == '\0') editor = var_get("EDITOR");
    if (editor == NULL || *editor == '\0') editor = "vi";
    size_t len = strlen(editor) + strlen(tmp) + 4;
    char *cmd = malloc(len);
    snprintf(cmd, len, "%s %s", editor, tmp);
    int status = run_string(cmd);
    free(cmd);
    if (status == 0) {
        FILE *in = fopen(tmp, "r");
        char *text = NULL;
        size_t tl = 0;
        if (in != NULL) {
            char chunk[4096];
            size_t got;
            while ((got = fread(chunk, 1, sizeof(chunk), in)) > 0) {
                char *grown = realloc(text, tl + got + 1);
                if (grown == NULL) break;
                text = grown;
                memcpy(text + tl, chunk, got);
                tl += got;
            }
            fclose(in);
        }
        if (text != NULL) {
            text[tl] = '\0';
            while (tl > 0 && text[tl - 1] == '\n') text[--tl] = '\0';
            if (last == count - 2) remove_at(count - 1);
            printf("%s\n", text);
            fflush(stdout);
            open_entry = 0;
            history_begin(text);
            open_entry = 0;
            status = run_string(text);
            free(text);
        }
    }
    unlink(tmp);
    return status;
}
