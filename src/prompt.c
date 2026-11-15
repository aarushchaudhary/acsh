#define _DEFAULT_SOURCE           /* gethostname */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>
#include "acsh.h"
#include "syntax.h"

/* Prompts. PS1 and PS2 take bash's backslash escapes, then parameter,
 * command and arithmetic expansion (as bash and ash do):
 *
 *   \u user   \h host   \H full host   \w directory (~ for $HOME)
 *   \W its last part   \$ # for root, else $   \g " (branch)" in a git
 *   repository, read from .git/HEAD without running git   \? last exit
 *   status   \j jobs   \! history number   \# command number   \s shell
 *   \v version   \t \T \@ \A \d \D{fmt} times   \n \r \a \e \\ \NNN
 *   \[ \] around text that takes no room (colour codes)
 *
 * PS1 and PS2 are never exported (vars.c), so a prompt written with
 * these escapes cannot break another shell started from acsh. */

#define ACSH_VERSION "2.0.0"

/* Around text that takes no room on screen; stripped when printing,
 * used by the line editor to measure the prompt. */
#define NOWIDTH_START '\x1c'
#define NOWIDTH_END   '\x1d'

typedef struct { char *s; size_t len, cap; } Text;

static void tput(Text *t, char c) {
    if (t->len + 2 > t->cap) {
        t->cap = t->cap ? t->cap * 2 : 128;
        t->s = realloc(t->s, t->cap);
    }
    t->s[t->len++] = c;
    t->s[t->len] = '\0';
}

/* Text an escape produced: protected from the expansion that follows. */
static void tquote(Text *t, const char *s) {
    for (; *s; s++) {
        if (strchr("$`\"\\", *s)) tput(t, '\\');
        tput(t, *s);
    }
}

/* The current git branch: walks up from the working directory to a .git
 * directory (or a .git file pointing elsewhere, as in worktrees and
 * submodules) and reads HEAD. Empty outside a repository. */
static void git_branch(char *out, size_t size) {
    out[0] = '\0';
    char dir[4096];
    if (getcwd(dir, sizeof(dir)) == NULL) return;
    for (;;) {
        char path[4200], head[8400];
        struct stat st;
        snprintf(path, sizeof(path), "%s/.git", dir);
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                snprintf(head, sizeof(head), "%s/HEAD", path);
            } else {          /* "gitdir: <path>" */
                FILE *f = fopen(path, "r");
                char line[4096] = "";
                if (f == NULL || fgets(line, sizeof(line), f) == NULL || strncmp(line, "gitdir: ", 8) != 0) {
                    if (f) fclose(f);
                    return;
                }
                fclose(f);
                line[strcspn(line, "\n")] = '\0';
                if (line[8] == '/') snprintf(head, sizeof(head), "%s/HEAD", line + 8);
                else snprintf(head, sizeof(head), "%s/%s/HEAD", dir, line + 8);
            }
            FILE *f = fopen(head, "r");
            char ref[512] = "";
            if (f == NULL) return;
            if (fgets(ref, sizeof(ref), f) == NULL) ref[0] = '\0';
            fclose(f);
            ref[strcspn(ref, "\n")] = '\0';
            if (strncmp(ref, "ref: refs/heads/", 16) == 0) snprintf(out, size, "%s", ref + 16);
            else if (strncmp(ref, "ref: ", 5) == 0) snprintf(out, size, "%s", ref + 5);
            else if (ref[0]) snprintf(out, size, "%.7s", ref);   /* detached: the short hash */
            return;
        }
        char *slash = strrchr(dir, '/');
        if (slash == NULL || slash == dir) {
            if (strcmp(dir, "/") == 0) return;
            strcpy(dir, "/");
        } else {
            *slash = '\0';
        }
    }
}

static int command_number = 0;

char *prompt_expand(const char *ps) {
    Text t = { NULL, 0, 0 };
    tput(&t, 'x');
    t.len = 0;
    t.s[0] = '\0';
    char buf[4200];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    for (const char *p = ps; *p; p++) {
        if (*p != '\\' || p[1] == '\0') {
            tput(&t, *p);
            continue;
        }
        char c = *++p;
        buf[0] = '\0';
        switch (c) {
        case 'u': {
            struct passwd *pw = getpwuid(geteuid());
            const char *u = pw ? pw->pw_name : var_get("USER");
            tquote(&t, u ? u : "?");
            break;
        }
        case 'h': case 'H':
            if (gethostname(buf, 256) != 0) strcpy(buf, "localhost");
            buf[255] = '\0';
            if (c == 'h') buf[strcspn(buf, ".")] = '\0';
            tquote(&t, buf);
            break;
        case 'w': case 'W': {
            const char *pwd = var_get("PWD");
            if (pwd == NULL || *pwd == '\0') pwd = getcwd(buf + 2048, 2048) ? buf + 2048 : "?";
            const char *home = var_get("HOME");
            size_t hl = home ? strlen(home) : 0;
            if (c == 'W') {
                const char *base = strrchr(pwd, '/');
                if (hl > 1 && strcmp(pwd, home) == 0) tquote(&t, "~");
                else tquote(&t, (base && base[1]) ? base + 1 : pwd);
            } else if (hl > 1 && strncmp(pwd, home, hl) == 0 && (pwd[hl] == '/' || pwd[hl] == '\0')) {
                tquote(&t, "~");
                tquote(&t, pwd + hl);
            } else {
                tquote(&t, pwd);
            }
            break;
        }
        case '$': tput(&t, geteuid() == 0 ? '#' : '$'); break;
        case 'g':
            git_branch(buf, 512);
            if (buf[0]) {
                tquote(&t, " (");
                tquote(&t, buf);
                tquote(&t, ")");
            }
            break;
        case '?': snprintf(buf, 32, "%d", env_get_last_status()); tquote(&t, buf); break;
        case 'j': {
            int n = 0;
            for (int i = 0; i < jobs_capacity(); i++) if (jobs_at(i) != NULL) n++;
            snprintf(buf, 32, "%d", n);
            tquote(&t, buf);
            break;
        }
        case '!': snprintf(buf, 32, "%d", history_number()); tquote(&t, buf); break;
        case '#': snprintf(buf, 32, "%d", command_number); tquote(&t, buf); break;
        case 's': tquote(&t, "acsh"); break;
        case 'v': case 'V': tquote(&t, ACSH_VERSION); break;
        case 't': strftime(buf, 64, "%H:%M:%S", &tm); tquote(&t, buf); break;
        case 'T': strftime(buf, 64, "%I:%M:%S", &tm); tquote(&t, buf); break;
        case '@': strftime(buf, 64, "%I:%M %p", &tm); tquote(&t, buf); break;
        case 'A': strftime(buf, 64, "%H:%M", &tm); tquote(&t, buf); break;
        case 'd': strftime(buf, 64, "%a %b %d", &tm); tquote(&t, buf); break;
        case 'D':
            if (p[1] == '{') {
                const char *end = strchr(p + 2, '}');
                if (end != NULL) {
                    char fmt[256];
                    size_t n = (size_t)(end - (p + 2));
                    if (n >= sizeof(fmt)) n = sizeof(fmt) - 1;
                    memcpy(fmt, p + 2, n);
                    fmt[n] = '\0';
                    if (strftime(buf, 256, n ? fmt : "%X", &tm) == 0) buf[0] = '\0';
                    tquote(&t, buf);
                    p = end;
                    break;
                }
            }
            tquote(&t, "\\D");
            break;
        case 'n': tput(&t, '\n'); break;
        case 'r': tput(&t, '\r'); break;
        case 'a': tput(&t, '\a'); break;
        case 'e': tput(&t, '\033'); break;
        case '\\': tquote(&t, "\\"); break;
        case '[': tput(&t, NOWIDTH_START); break;
        case ']': tput(&t, NOWIDTH_END); break;
        default:
            if (c >= '0' && c <= '7') {
                int v = c - '0';
                for (int k = 0; k < 2 && p[1] >= '0' && p[1] <= '7'; k++) v = v * 8 + (*++p - '0');
                char one[2] = { (char)v, '\0' };
                if (v) tquote(&t, one);
            } else {
                tput(&t, '\\');   /* unknown: keep it for the expansion (\$ and the like) */
                tput(&t, c);
            }
        }
    }
    /* then $VAR, $(cmd), $((...)), as inside double quotes */
    char *marked = lex_mark(t.s, 1);
    char *expanded = marked ? expand_string(marked, 0) : NULL;
    free(marked);
    if (expanded == NULL) {
        return t.s;   /* an expansion error was reported: show the text as is */
    }
    free(t.s);
    return expanded;
}

char *prompt_string(const char *which) {
    const char *ps = var_get(which);
    if (ps == NULL) ps = strcmp(which, "PS1") == 0 ? "acsh:\\w\\g\\$ " : "> ";  /* unset by the user */
    if (strcmp(which, "PS1") == 0) command_number++;
    return prompt_expand(ps);
}

void prompt_print(const char *which) {
    char *s = prompt_string(which);
    for (const char *c = s; *c; c++) {
        if (*c != NOWIDTH_START && *c != NOWIDTH_END) fputc(*c, stderr);
    }
    fflush(stderr);
    free(s);
}

size_t prompt_width(const char *s) {
    size_t w = 0;
    int hidden = 0;
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == NOWIDTH_START) hidden = 1;
        else if (*c == NOWIDTH_END) hidden = 0;
        else if (*c == '\n' || *c == '\r') w = 0;           /* only the last line counts */
        else if (*c == 0x1b && c[1] == '[') {               /* a colour code without \[ \]: no width */
            for (c += 2; *c && !(*c >= 0x40 && *c <= 0x7e); c++) {}
            if (*c == '\0') break;
        }
        else if (!hidden && (*c & 0xc0) != 0x80) w++;       /* UTF-8: count characters */
    }
    return w;
}
