#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "acsh.h"

/* The echo and printf builtins, matching busybox ash. */

/* Writes one backslash escape starting at s[0] == '\\' (as printf's
 * format and %b, or echo -e, understand them) and returns how many
 * characters it used. *stop is set by \c (end all output). In %b and
 * echo -e an octal escape is \0NNN; in a printf format it is \NNN. */
static size_t put_escape(const char *s, int zero_octal, int *stop) {
    const char *p = s + 1;
    switch (*p) {
    case 'a':  putchar('\a'); return 2;
    case 'b':  putchar('\b'); return 2;
    case 'c':  *stop = 1;     return 2;
    case 'e':  putchar(27);   return 2;
    case 'f':  putchar('\f'); return 2;
    case 'n':  putchar('\n'); return 2;
    case 'r':  putchar('\r'); return 2;
    case 't':  putchar('\t'); return 2;
    case 'v':  putchar('\v'); return 2;
    case '\\': putchar('\\'); return 2;
    case 'x': {
        int v = 0, n = 0;
        while (n < 2 && isxdigit((unsigned char)p[1 + n])) {
            char c = p[1 + n];
            v = v * 16 + (isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10);
            n++;
        }
        if (n == 0) { putchar('\\'); return 1; }
        putchar(v);
        return (size_t)(2 + n);
    }
    default:
        break;
    }
    if (*p >= '0' && *p <= '7') {
        const char *q = p;
        int max = 3;
        if (zero_octal && *q == '0') { q++; } /* \0NNN */
        int v = 0, n = 0;
        while (n < max && *q >= '0' && *q <= '7') {
            v = v * 8 + (*q - '0');
            q++;
            n++;
        }
        putchar(v);
        return (size_t)(q - s);
    }
    putchar('\\');
    return 1;
}

/* echo [-neE] [ARG...]: options only when an argument is made entirely
 * of n, e and E letters, as in ash. No escapes unless -e. */
int builtin_echo(Command *cmd) {
    int i = 1, newline = 1, escapes = 0;
    for (; i < cmd->argc; i++) {
        const char *a = cmd->argv[i];
        if (a[0] != '-' || a[1] == '\0' || strspn(a + 1, "neE") != strlen(a + 1)) break;
        for (const char *c = a + 1; *c; c++) {
            if (*c == 'n') newline = 0;
            else if (*c == 'e') escapes = 1;
            else escapes = 0;
        }
    }
    int stop = 0;
    for (int first = 1; i < cmd->argc && !stop; i++, first = 0) {
        if (!first) putchar(' ');
        const char *s = cmd->argv[i];
        while (*s && !stop) {
            if (escapes && *s == '\\') s += put_escape(s, 1, &stop);
            else putchar(*s++);
        }
    }
    if (newline && !stop) putchar('\n');
    fflush(stdout);
    return ferror(stdout) ? 1 : 0;
}

/* A numeric argument: decimal, 0x hex, 0 octal, or 'c / "c for the
 * character code. Reports and returns 0 if it is not a number. */
static long long numeric_arg(const char *s, int *status) {
    if (*s == '\'' || *s == '"') return (unsigned char)s[1];
    if (*s == '\0') return 0;
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 0);
    if (*end != '\0' || errno != 0) {
        shell_error("printf: invalid number '%s'\n", s);
        *status = 1;
    }
    return v;
}

static double float_arg(const char *s, int *status) {
    if (*s == '\0') return 0;
    char *end;
    double v = strtod(s, &end);
    if (*end != '\0') {
        shell_error("printf: invalid number '%s'\n", s);
        *status = 1;
    }
    return v;
}

/* printf FORMAT [ARG...]: the format is reused while arguments remain. */
int builtin_printf(Command *cmd) {
    if (cmd->argc < 2) {
        shell_error("printf: usage: printf FORMAT [ARG...]\n");
        return 2;
    }
    const char *format = cmd->argv[1];
    char **args = cmd->argv + 2;
    int nargs = cmd->argc - 2, used = 0, status = 0, stop = 0;

    do {
        int consumed_before = used;
        for (const char *f = format; *f && !stop;) {
            if (*f == '\\') {
                f += put_escape(f, 0, &stop);
                continue;
            }
            if (*f != '%') {
                putchar(*f++);
                continue;
            }
            if (f[1] == '%') {
                putchar('%');
                f += 2;
                continue;
            }
            /* %[flags][width][.precision]conversion */
            char spec[64];
            size_t k = 0;
            const char *start = f++;
            spec[k++] = '%';
            while (*f && strchr("-+ #0", *f) && k < 40) spec[k++] = *f++;
            if (*f == '*') {        /* width from the next argument */
                int width = used < nargs ? (int)numeric_arg(args[used++], &status) : 0;
                k += (size_t)snprintf(spec + k, sizeof(spec) - k, "%d", width);
                f++;
            } else {
                while (isdigit((unsigned char)*f) && k < 40) spec[k++] = *f++;
            }
            if (*f == '.') {
                spec[k++] = *f++;
                if (*f == '*') {    /* precision from the next argument */
                    int prec = used < nargs ? (int)numeric_arg(args[used++], &status) : 0;
                    k += (size_t)snprintf(spec + k, sizeof(spec) - k, "%d", prec);
                    f++;
                } else {
                    while (isdigit((unsigned char)*f) && k < 40) spec[k++] = *f++;
                }
            }
            char conv = *f;
            if (conv == '\0' || strchr("diouxXcsbfeEgG", conv) == NULL) {
                /* ash refuses anything else, e.g. %q or %z */
                const char *end = conv ? f + 1 : f;
                fflush(stdout);
                shell_error("printf: %.*s: invalid format\n", (int)(end - start), start);
                return 1;
            }
            f++;
            const char *arg = used < nargs ? args[used++] : NULL;
            switch (conv) {
            case 'd': case 'i':
                spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = conv; spec[k] = '\0';
                printf(spec, arg ? numeric_arg(arg, &status) : 0LL);
                break;
            case 'o': case 'u': case 'x': case 'X':
                spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = conv; spec[k] = '\0';
                printf(spec, (unsigned long long)(arg ? numeric_arg(arg, &status) : 0LL));
                break;
            case 'f': case 'e': case 'E': case 'g': case 'G':
                spec[k++] = conv; spec[k] = '\0';
                printf(spec, arg ? float_arg(arg, &status) : 0.0);
                break;
            case 'c':
                spec[k++] = 'c'; spec[k] = '\0';
                if (arg && *arg) printf(spec, *arg);
                else if (k > 2) printf(spec, ' ');  /* width with no character */
                break;
            case 's':
                spec[k++] = 's'; spec[k] = '\0';
                printf(spec, arg ? arg : "");
                break;
            case 'b': {
                /* the argument's escapes are interpreted, as by echo -e */
                const char *s = arg ? arg : "";
                while (*s && !stop) {
                    if (*s == '\\') s += put_escape(s, 1, &stop);
                    else putchar(*s++);
                }
                break;
            }
            }
        }
        if (used == consumed_before) break; /* the format takes no arguments */
    } while (used < nargs && !stop);

    fflush(stdout);
    return status;
}
