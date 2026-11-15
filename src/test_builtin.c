#define _DEFAULT_SOURCE           /* S_ISVTX under -std=c11 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fnmatch.h>
#include <regex.h>
#include <unistd.h>
#include <sys/stat.h>
#include "acsh.h"

/* test / [ / [[: POSIX's rules by argument count, then a full
 * precedence parser for longer expressions:
 *
 *   or  : and ( -o and )*        and  : not ( -a not )*
 *   not : ! not | primary        primary : ( or ) | -op ARG | ARG op ARG | ARG
 *
 * [[ ]] is test plus what ash's [[ adds: && and || between tests,
 * == and != matching shell patterns, and =~ matching an extended
 * regular expression. Like ash (and unlike bash), its words are split
 * and globbed as in any command. acsh also accepts ( ) for grouping
 * and < > as string comparisons inside [[ ]]. */

typedef struct {
    char **argv;
    int    argc;
    int    pos;
    int    dbl;    /* [[ ]] */
    int    error;  /* 2 after a reported error */
} Test;

static void test_error(Test *t, const char *fmt, const char *arg) {
    if (!t->error) {
        shell_error_prefix();
        fprintf(stderr, fmt, arg);
        fputc('\n', stderr);
    }
    t->error = 2;
}

static int is_unary(const char *op) {
    return op[0] == '-' && op[1] != '\0' && op[2] == '\0' && strchr("bcdefgGhkLnOprsStuwxz", op[1]) != NULL;
}

static int is_binary(Test *t, const char *op) {
    static const char *ops[] = { "=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt",
                                 "-ge", "-nt", "-ot", "-ef", NULL };
    for (int i = 0; ops[i] != NULL; i++) {
        if (strcmp(op, ops[i]) == 0) return 1;
    }
    return t->dbl && strcmp(op, "=~") == 0;
}

static long long number(Test *t, const char *s) {
    char *end;
    errno = 0;
    while (*s == ' ' || *s == '\t') s++;
    long long v = strtoll(s, &end, 10);
    while (*end == ' ' || *end == '\t') end++;
    if (*s == '\0' || *end != '\0' || errno != 0) {
        test_error(t, "%s: bad number", s);
        return 0;
    }
    return v;
}

static int unary(Test *t, const char *op, const char *arg) {
    struct stat st;
    char c = op[1];
    if (c == 'n') return *arg != '\0';
    if (c == 'z') return *arg == '\0';
    if (c == 't') return isatty((int)number(t, arg));
    if (c == 'h' || c == 'L') return lstat(arg, &st) == 0 && S_ISLNK(st.st_mode);
    if (c == 'r') return access(arg, R_OK) == 0;
    if (c == 'w') return access(arg, W_OK) == 0;
    if (c == 'x') return access(arg, X_OK) == 0;
    if (stat(arg, &st) != 0) return 0;
    switch (c) {
    case 'b': return S_ISBLK(st.st_mode);
    case 'c': return S_ISCHR(st.st_mode);
    case 'd': return S_ISDIR(st.st_mode);
    case 'e': return 1;
    case 'f': return S_ISREG(st.st_mode);
    case 'g': return (st.st_mode & S_ISGID) != 0;
    case 'G': return st.st_gid == getegid();
    case 'k': return (st.st_mode & S_ISVTX) != 0;
    case 'O': return st.st_uid == geteuid();
    case 'p': return S_ISFIFO(st.st_mode);
    case 's': return st.st_size > 0;
    case 'S': return S_ISSOCK(st.st_mode);
    case 'u': return (st.st_mode & S_ISUID) != 0;
    }
    return 0;
}

static int binary(Test *t, const char *a, const char *op, const char *b) {
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
        return t->dbl ? fnmatch(b, a, 0) == 0 : strcmp(a, b) == 0;
    }
    if (strcmp(op, "!=") == 0) {
        return t->dbl ? fnmatch(b, a, 0) != 0 : strcmp(a, b) != 0;
    }
    if (strcmp(op, "<") == 0) return strcmp(a, b) < 0;
    if (strcmp(op, ">") == 0) return strcmp(a, b) > 0;
    if (strcmp(op, "=~") == 0) {
        regex_t re;
        if (regcomp(&re, b, REG_EXTENDED | REG_NOSUB) != 0) {
            test_error(t, "%s: invalid regular expression", b);
            return 0;
        }
        int match = regexec(&re, a, 0, NULL, 0) == 0;
        regfree(&re);
        return match;
    }
    if (op[1] == 'n' || op[1] == 'o' || (op[1] == 'e' && op[2] == 'f')) {
        struct stat sa, sb;
        int ha = stat(a, &sa) == 0, hb = stat(b, &sb) == 0;
        if (strcmp(op, "-ef") == 0) return ha && hb && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
        if (strcmp(op, "-nt") == 0) return ha && (!hb || sa.st_mtime > sb.st_mtime);
        return hb && (!ha || sa.st_mtime < sb.st_mtime); /* -ot */
    }
    long long x = number(t, a), y = number(t, b);
    if (strcmp(op, "-eq") == 0) return x == y;
    if (strcmp(op, "-ne") == 0) return x != y;
    if (strcmp(op, "-lt") == 0) return x < y;
    if (strcmp(op, "-le") == 0) return x <= y;
    if (strcmp(op, "-gt") == 0) return x > y;
    return x >= y; /* -ge */
}

static const char *peek(Test *t, int k) {
    return (t->pos + k < t->argc) ? t->argv[t->pos + k] : NULL;
}

static int is_or(Test *t, const char *s)  { return s && (strcmp(s, "-o") == 0 || (t->dbl && strcmp(s, "||") == 0)); }
static int is_and(Test *t, const char *s) { return s && (strcmp(s, "-a") == 0 || (t->dbl && strcmp(s, "&&") == 0)); }

static int parse_or(Test *t);

static int parse_primary(Test *t) {
    const char *a = peek(t, 0);
    if (a == NULL) {
        test_error(t, "%s", "test: argument expected");
        return 0;
    }
    /* ARG op ARG comes first, so that `test ( = (` compares strings */
    const char *op = peek(t, 1);
    if (op != NULL && is_binary(t, op) && peek(t, 2) != NULL) {
        t->pos += 3;
        return binary(t, a, op, t->argv[t->pos - 1]);
    }
    if (strcmp(a, "(") == 0) {
        t->pos++;
        int v = parse_or(t);
        if (peek(t, 0) == NULL || strcmp(peek(t, 0), ")") != 0) {
            test_error(t, "%s", "test: closing ) expected");
            return 0;
        }
        t->pos++;
        return v;
    }
    if (is_unary(a) && peek(t, 1) != NULL) {
        t->pos += 2;
        return unary(t, a, t->argv[t->pos - 1]);
    }
    t->pos++;
    return *a != '\0';
}

static int parse_not(Test *t) {
    if (stack_exhausted()) {
        test_error(t, "%s", "test: expression nested too deeply");
        return 0;
    }
    const char *a = peek(t, 0);
    if (a != NULL && strcmp(a, "!") == 0 && peek(t, 1) != NULL) {
        t->pos++;
        return !parse_not(t);
    }
    return parse_primary(t);
}

static int parse_and(Test *t) {
    int v = parse_not(t);
    while (is_and(t, peek(t, 0))) {
        t->pos++;
        int r = parse_not(t);
        v = v && r;
    }
    return v;
}

static int parse_or(Test *t) {
    int v = parse_and(t);
    while (is_or(t, peek(t, 0))) {
        t->pos++;
        int r = parse_and(t);
        v = v || r;
    }
    return v;
}

/* POSIX's rules for up to four arguments, which settle cases a general
 * parser would read differently (test ! = x, test -n, ...). */
static int evaluate(Test *t, int start, int n) {
    char **a = t->argv + start;
    switch (n) {
    case 0:
        return 0;
    case 1:
        return *a[0] != '\0';
    case 2:
        if (strcmp(a[0], "!") == 0) return *a[1] == '\0';
        if (is_unary(a[0])) return unary(t, a[0], a[1]);
        break;
    case 3:
        if (is_binary(t, a[1])) return binary(t, a[0], a[1], a[2]);
        if (strcmp(a[0], "!") == 0) return !evaluate(t, start + 1, 2);
        if (strcmp(a[0], "(") == 0 && strcmp(a[2], ")") == 0) return *a[1] != '\0';
        if (is_and(t, a[1])) return *a[0] != '\0' && *a[2] != '\0';
        if (is_or(t, a[1])) return *a[0] != '\0' || *a[2] != '\0';
        break;
    case 4:
        if (strcmp(a[0], "!") == 0) return !evaluate(t, start + 1, 3);
        if (strcmp(a[0], "(") == 0 && strcmp(a[3], ")") == 0) return evaluate(t, start + 1, 2);
        break;
    }
    t->pos = start;
    int v = parse_or(t);
    if (!t->error && t->pos < start + n) {
        test_error(t, "%s: unknown operand", t->argv[t->pos]);
    }
    return v;
}

int builtin_test(Command *cmd) {
    const char *name = cmd->argv[0];
    int argc = cmd->argc;
    Test t = { cmd->argv, argc, 1, 0, 0 };
    if (strcmp(name, "[") == 0 || strcmp(name, "[[") == 0) {
        const char *close = (name[1] == '[') ? "]]" : "]";
        if (argc < 2 || strcmp(cmd->argv[argc - 1], close) != 0) {
            shell_error("%s: missing %s\n", name, close);
            return 2;
        }
        t.dbl = (name[1] == '[');
        argc--;
        t.argc = argc;
    }
    int v = evaluate(&t, 1, argc - 1);
    if (t.error) {
        return 2;
    }
    return v ? 0 : 1;
}
