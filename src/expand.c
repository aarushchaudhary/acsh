#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <pwd.h>
#include "acsh.h"
#include "syntax.h"

/* Word expansion, in POSIX order: tilde, parameters ($x, ${x...}),
 * command substitution, arithmetic, then field splitting on $IFS,
 * pathname expansion (globbing) and quote removal.
 *
 * Expansion builds a buffer in which every character carries where it
 * came from, because that decides what may happen to it next:
 *
 *   A_LIT    typed unquoted text: globbed, never split
 *   A_EXP    the unquoted result of an expansion: split on $IFS and globbed
 *   A_QUO    quoted text or the result of a quoted expansion: neither
 *   A_BREAK  a field boundary (between the words of "$@")
 *   A_KEEP   zero-width: a field exists here even if empty ("" or '') */

enum { A_LIT, A_EXP, A_QUO, A_BREAK, A_KEEP };

typedef struct {
    char  *c;     /* characters */
    char  *a;     /* their attributes */
    size_t n, cap;
} Buf;

static int failed;  /* an expansion error was reported (${x?}, $((1/0)), ...) */

static void bput(Buf *b, char ch, char attr) {
    if (b->n + 1 >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->c = realloc(b->c, b->cap);
        b->a = realloc(b->a, b->cap);
        if (b->c == NULL || b->a == NULL) {
            shell_error("out of memory\n");
            exit(2);
        }
    }
    b->c[b->n] = ch;
    b->a[b->n] = attr;
    b->n++;
}

static void bputs(Buf *b, const char *s, char attr) {
    for (; *s; s++) bput(b, *s, attr);
}

static void bfree(Buf *b) {
    free(b->c);
    free(b->a);
    b->c = b->a = NULL;
    b->n = b->cap = 0;
}

/* The characters of b as a plain heap string (markers dropped). */
static char *bstring(const Buf *b) {
    char *s = malloc(b->n + 1);
    size_t k = 0;
    for (size_t i = 0; i < b->n; i++) {
        if (b->a[i] == A_KEEP) continue;
        s[k++] = (b->a[i] == A_BREAK) ? ' ' : b->c[i];
    }
    s[k] = '\0';
    return s;
}

static void fail(const char *fmt, const char *arg) {
    shell_error_prefix();
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
    failed = 1;
}

/* ---- scanning raw regions copied verbatim by the lexer ---- */

/* Given s[i] == '(' (just after a '$'), returns the index of the
 * matching ')'. Quotes and backslashes inside are respected. */
static size_t match_paren(const char *s, size_t i) {
    ParenScan ps;
    paren_scan_init(&ps);
    int quote = 0;
    for (i++; s[i]; i++) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && (quote == '"' || quote == 'D') && s[i + 1]) i++;
            else if (c == (quote == 'D' ? '\'' : quote)) quote = 0;
        } else if (c == '\\' && s[i + 1]) {
            i++;
            paren_scan(&ps, 'x');
        } else if (c == '\'' && i > 0 && s[i - 1] == '$') {
            quote = 'D'; /* $'...': backslash escapes the quote */
            paren_scan(&ps, c);
        } else if (c == '\'' || c == '"') {
            quote = c;
            paren_scan(&ps, c);
        } else if (paren_scan(&ps, (unsigned char)c)) {
            return i;
        }
    }
    return i;
}

/* Given s[i] == '{', returns the index of the matching '}'. */
static size_t match_brace(const char *s, size_t i) {
    int depth = 0, quote = 0;
    for (; s[i]; i++) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && quote == '"' && s[i + 1]) i++;
            else if (c == quote) quote = 0;
        } else if (c == '\\' && s[i + 1]) {
            i++;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '{') {
            depth++;
        } else if (c == '}' && --depth == 0) {
            return i;
        }
    }
    return i;
}

/* ---- arithmetic: $(( expr )) ---- */

typedef struct {
    const char *s;
    size_t      p;
    int         skip;  /* inside an unevaluated branch: no side effects, no errors */
} Arith;

static long arith_assign(Arith *ar);
static long arith_comma(Arith *ar);

/* 64-bit two's-complement arithmetic that wraps on overflow, as ash and
 * bash do, without C's undefined behavior: done in unsigned, shift
 * counts taken mod 64, and LONG_MIN / -1 (a trap on x86) as negation. */
static long w_add(long a, long b) { return (long)((unsigned long)a + (unsigned long)b); }
static long w_sub(long a, long b) { return (long)((unsigned long)a - (unsigned long)b); }
static long w_mul(long a, long b) { return (long)((unsigned long)a * (unsigned long)b); }
static long w_neg(long a)         { return (long)(0UL - (unsigned long)a); }
static long w_shl(long a, long b) { return (long)((unsigned long)a << (b & 63)); }
static long w_shr(long a, long b) { return a >> (b & 63); }
static long w_div(long a, long b) { return b == -1 ? w_neg(a) : a / b; }
static long w_mod(long a, long b) { return b == -1 ? 0 : a % b; }

static void ws(Arith *ar) {
    while (isspace((unsigned char)ar->s[ar->p])) ar->p++;
}

static int at(Arith *ar, const char *op) {
    ws(ar);
    if (ar->s[ar->p] != op[0]) return 0; /* the common case, cheaply */
    return strncmp(ar->s + ar->p, op, strlen(op)) == 0;
}

static int take_op(Arith *ar, const char *op) {
    if (at(ar, op)) {
        ar->p += strlen(op);
        return 1;
    }
    return 0;
}

static void arith_fail(Arith *ar, const char *msg) {
    if (!ar->skip && !failed) fail("%s", msg);
    failed = 1;
}

/* A variable's value in arithmetic: a number, or else an expression
 * evaluated in turn (x=3+4; $((x*2)) is 14, as in ash and bash). */
static long var_value(Arith *ar, const char *name) {
    static int nesting = 0;
    const char *v = var_get(name);
    if (v == NULL || *v == '\0') return 0;
    char *end;
    errno = 0;
    long n = strtol(v, &end, 0);
    while (isspace((unsigned char)*end)) end++;
    if (*end == '\0' && errno == 0) return n;
    if (nesting >= 64) {
        arith_fail(ar, "expression recursion level exceeded");
        return 0;
    }
    char *copy = strdup(v); /* evaluating may reassign the variable */
    Arith sub = { copy, 0, ar->skip };
    nesting++;
    ws(&sub);
    n = (sub.s[sub.p] == '\0') ? 0 : arith_comma(&sub);
    ws(&sub);
    if (sub.s[sub.p] != '\0') arith_fail(ar, "arithmetic syntax error");
    nesting--;
    free(copy);
    return n;
}

static void set_var(Arith *ar, const char *name, long v) {
    if (ar->skip) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", v);
    if (var_set(name, buf) != 0) failed = 1;
}

static int read_name(Arith *ar, char *name, size_t size) {
    ws(ar);
    size_t k = 0;
    if (!(isalpha((unsigned char)ar->s[ar->p]) || ar->s[ar->p] == '_')) return 0;
    while ((isalnum((unsigned char)ar->s[ar->p]) || ar->s[ar->p] == '_') && k + 1 < size) {
        name[k++] = ar->s[ar->p++];
    }
    name[k] = '\0';
    return 1;
}

static long arith_primary(Arith *ar) {
    ws(ar);
    char c = ar->s[ar->p];
    if (stack_exhausted()) {
        arith_fail(ar, "arithmetic expression nested too deeply");
        return 0;
    }
    if (c == '(') {
        ar->p++;
        long v = arith_comma(ar);
        if (!take_op(ar, ")")) arith_fail(ar, "arithmetic syntax error: missing ')'");
        return v;
    }
    if (isdigit((unsigned char)c)) {
        char *end;
        long v = strtol(ar->s + ar->p, &end, 0);
        if (*end == '#' && v >= 2 && v <= 64) {
            /* BASE#DIGITS: 0-9, a-z, A-Z, @, _ as in ash and bash */
            long base = v;
            v = 0;
            const char *d = end + 1;
            for (;; d++) {
                int digit;
                if (isdigit((unsigned char)*d)) digit = *d - '0';
                else if (*d >= 'a' && *d <= 'z') digit = *d - 'a' + 10;
                else if (*d >= 'A' && *d <= 'Z') digit = base <= 36 ? *d - 'A' + 10 : *d - 'A' + 36;
                else if (*d == '@') digit = 62;
                else if (*d == '_') digit = 63;
                else break;
                if (digit >= base) break;
                v = w_add(w_mul(v, base), digit);
            }
            end = (char *)d;
        }
        ar->p = (size_t)(end - ar->s);
        if (isalnum((unsigned char)ar->s[ar->p])) arith_fail(ar, "arithmetic syntax error");
        return v;
    }
    char name[256];
    if (read_name(ar, name, sizeof(name))) {
        long v = var_value(ar, name);
        if (take_op(ar, "++")) { set_var(ar, name, w_add(v, 1)); return v; }
        if (take_op(ar, "--")) { set_var(ar, name, w_sub(v, 1)); return v; }
        return v;
    }
    arith_fail(ar, "arithmetic syntax error");
    return 0;
}

static long arith_unary(Arith *ar) {
    if (stack_exhausted()) {
        arith_fail(ar, "arithmetic expression nested too deeply");
        return 0;
    }
    char name[256];
    int inc = take_op(ar, "++");
    int dec = !inc && take_op(ar, "--");
    if (inc || dec) {
        size_t save = ar->p;
        if (read_name(ar, name, sizeof(name))) {
            long v = w_add(var_value(ar, name), inc ? 1 : -1);
            set_var(ar, name, v);
            return v;
        }
        ar->p = save;
        arith_fail(ar, "arithmetic syntax error");
        return 0;
    }
    if (take_op(ar, "+")) return arith_unary(ar);
    if (take_op(ar, "-")) return w_neg(arith_unary(ar));
    if (take_op(ar, "!")) return !arith_unary(ar);
    if (take_op(ar, "~")) return ~arith_unary(ar);
    return arith_primary(ar);
}

/* a ** b: right-associative, binds tighter than * but looser than
 * unary minus (-2**2 is 4, as in ash and bash). */
static long arith_pow(Arith *ar) {
    long v = arith_unary(ar);
    if (!at(ar, "**") || ar->s[ar->p + 2] == '=') return v;
    ar->p += 2;
    long e = arith_pow(ar);
    if (e < 0) {
        arith_fail(ar, "exponent less than 0");
        return 0;
    }
    long r = 1;
    for (; e > 0; e >>= 1) {   /* by squaring: a huge exponent stays fast */
        if (e & 1) r = w_mul(r, v);
        v = w_mul(v, v);
    }
    return r;
}

static long arith_mul(Arith *ar) {
    long v = arith_pow(ar);
    for (;;) {
        if (at(ar, "*=") || at(ar, "/=") || at(ar, "%=")) return v;
        int op = 0;
        if (take_op(ar, "*")) op = '*';
        else if (take_op(ar, "/")) op = '/';
        else if (take_op(ar, "%")) op = '%';
        else return v;
        long r = arith_pow(ar);
        if (op == '*') { v = w_mul(v, r); continue; }
        if (r == 0) {
            arith_fail(ar, "divide by zero");
            v = 0;
            continue;
        }
        v = (op == '/') ? w_div(v, r) : w_mod(v, r);
    }
}

static long arith_add(Arith *ar) {
    long v = arith_mul(ar);
    for (;;) {
        if (at(ar, "+=") || at(ar, "-=") || at(ar, "++") || at(ar, "--")) return v;
        if (take_op(ar, "+")) v = w_add(v, arith_mul(ar));
        else if (take_op(ar, "-")) v = w_sub(v, arith_mul(ar));
        else return v;
    }
}

static long arith_shift(Arith *ar) {
    long v = arith_add(ar);
    for (;;) {
        if (at(ar, "<<=") || at(ar, ">>=")) return v;
        if (take_op(ar, "<<")) v = w_shl(v, arith_add(ar));
        else if (take_op(ar, ">>")) v = w_shr(v, arith_add(ar));
        else return v;
    }
}

static long arith_rel(Arith *ar) {
    long v = arith_shift(ar);
    for (;;) {
        if (take_op(ar, "<=")) v = v <= arith_shift(ar);
        else if (take_op(ar, ">=")) v = v >= arith_shift(ar);
        else if (!at(ar, "<<") && take_op(ar, "<")) v = v < arith_shift(ar);
        else if (!at(ar, ">>") && take_op(ar, ">")) v = v > arith_shift(ar);
        else return v;
    }
}

static long arith_eq(Arith *ar) {
    long v = arith_rel(ar);
    for (;;) {
        if (take_op(ar, "==")) v = v == arith_rel(ar);
        else if (take_op(ar, "!=")) v = v != arith_rel(ar);
        else return v;
    }
}

static long arith_band(Arith *ar) {
    long v = arith_eq(ar);
    while (!at(ar, "&&") && !at(ar, "&=") && take_op(ar, "&")) v &= arith_eq(ar);
    return v;
}

static long arith_bxor(Arith *ar) {
    long v = arith_band(ar);
    while (!at(ar, "^=") && take_op(ar, "^")) v ^= arith_band(ar);
    return v;
}

static long arith_bor(Arith *ar) {
    long v = arith_bxor(ar);
    while (!at(ar, "||") && !at(ar, "|=") && take_op(ar, "|")) v |= arith_bxor(ar);
    return v;
}

static long arith_land(Arith *ar) {
    long v = arith_bor(ar);
    while (take_op(ar, "&&")) {
        int skip = ar->skip;
        if (!v) ar->skip = 1;
        long r = arith_bor(ar);
        ar->skip = skip;
        v = v && r;
    }
    return v;
}

static long arith_lor(Arith *ar) {
    long v = arith_land(ar);
    while (take_op(ar, "||")) {
        int skip = ar->skip;
        if (v) ar->skip = 1;
        long r = arith_land(ar);
        ar->skip = skip;
        v = v || r;
    }
    return v;
}

static long arith_ternary(Arith *ar) {
    if (stack_exhausted()) {
        arith_fail(ar, "arithmetic expression nested too deeply");
        return 0;
    }
    long c = arith_lor(ar);
    if (!take_op(ar, "?")) return c;
    int skip = ar->skip;
    ar->skip = skip || !c;
    long a = arith_assign(ar);
    if (!take_op(ar, ":")) arith_fail(ar, "arithmetic syntax error: expected ':'");
    ar->skip = skip || c;
    long b = arith_assign(ar);
    ar->skip = skip;
    return c ? a : b;
}

static long arith_assign(Arith *ar) {
    if (stack_exhausted()) {
        arith_fail(ar, "arithmetic expression nested too deeply");
        return 0;
    }
    size_t save = ar->p;
    char name[256];
    if (read_name(ar, name, sizeof(name))) {
        static const char *ops[] = { "<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=", "=" };
        for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
            if (at(ar, ops[i]) && !(ops[i][0] == '=' && ar->s[ar->p + 1] == '=')) {
                ar->p += strlen(ops[i]);
                long r = arith_assign(ar);
                long v = (ops[i][0] == '=') ? 0 : var_value(ar, name); /* x=1 ignores the old value */
                switch (ops[i][0]) {
                case '=': v = r; break;
                case '+': v = w_add(v, r); break;
                case '-': v = w_sub(v, r); break;
                case '*': v = w_mul(v, r); break;
                case '/': case '%':
                    if (r == 0) { arith_fail(ar, "divide by zero"); return 0; }
                    v = (ops[i][0] == '/') ? w_div(v, r) : w_mod(v, r);
                    break;
                case '<': v = w_shl(v, r); break;
                case '>': v = w_shr(v, r); break;
                case '&': v &= r; break;
                case '^': v ^= r; break;
                case '|': v |= r; break;
                }
                set_var(ar, name, v);
                return v;
            }
        }
        ar->p = save;
    }
    return arith_ternary(ar);
}

/* a, b: evaluates both, the value is b's. */
static long arith_comma(Arith *ar) {
    long v = arith_assign(ar);
    while (take_op(ar, ",")) v = arith_assign(ar);
    return v;
}

/* Evaluates an (already expanded) arithmetic expression. */
static long arith_eval(const char *text) {
    Arith ar = { text, 0, 0 };
    ws(&ar);
    if (ar.s[ar.p] == '\0') return 0;
    long v = arith_comma(&ar);
    ws(&ar);
    if (ar.s[ar.p] != '\0') arith_fail(&ar, "arithmetic syntax error");
    return v;
}

int arith_evaluate(const char *expr, long *result) {
    failed = 0;
    *result = arith_eval(expr);
    return failed ? -1 : 0;
}

/* ---- the expander ---- */

static void expand_into(Buf *b, const char *w, int lit_as_exp, int assign);

/* Expands raw text (an operator word in ${x:-word}) into b. */
static void expand_raw(Buf *b, const char *raw, int dq, int lit_as_exp) {
    char *marked = lex_mark(raw, dq);
    if (marked == NULL) {
        failed = 1;
        return;
    }
    expand_into(b, marked, lit_as_exp, 0);
    free(marked);
}

/* Expands raw text to a plain string (no splitting or globbing). */
static char *raw_to_string(const char *raw, int dq) {
    Buf b = { 0 };
    expand_raw(&b, raw, dq, 0);
    char *s = bstring(&b);
    bfree(&b);
    return s;
}

/* Expands raw text to an fnmatch() pattern: quoted characters are
 * escaped so they match only themselves. */
static char *raw_to_pattern(const char *raw) {
    Buf b = { 0 };
    expand_raw(&b, raw, 0, 0);
    char *p = malloc(b.n * 2 + 1);
    size_t k = 0;
    for (size_t i = 0; i < b.n; i++) {
        if (b.a[i] == A_KEEP || b.a[i] == A_BREAK) continue;
        if (b.a[i] == A_QUO && strchr("*?[]\\", b.c[i])) p[k++] = '\\';
        p[k++] = b.c[i];
    }
    p[k] = '\0';
    bfree(&b);
    return p;
}

static const char *ifs_value(void) {
    const char *ifs = var_get("IFS");
    return ifs != NULL ? ifs : " \t\n";
}

/* $@ / $* (and ${@} ${*}): each parameter a field when unquoted or in
 * "$@"; joined by the first character of $IFS in "$*". */
static void put_params(Buf *b, int star, int dq, int *empty_at) {
    int n = env_positional_count();
    if (dq && star) {
        const char *ifs = ifs_value();
        for (int i = 1; i <= n; i++) {
            if (i > 1 && *ifs) bput(b, *ifs, A_QUO);
            bputs(b, env_positional(i), A_QUO);
        }
        return;
    }
    if (n == 0 && dq) {
        *empty_at = 1; /* "$@" with no parameters: no field at all */
        return;
    }
    for (int i = 1; i <= n; i++) {
        if (i > 1) bput(b, ' ', A_BREAK);
        if (dq) bput(b, ' ', A_KEEP);
        bputs(b, env_positional(i), dq ? A_QUO : A_EXP);
    }
}

static int is_name_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int is_name_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* Removes the shortest/longest prefix/suffix of value matching pattern. */
static char *trim(const char *value, const char *pattern, int suffix, int longest) {
    size_t len = strlen(value);
    char *tmp = strdup(value);
    if (!suffix) {
        for (size_t k = 0; k <= len; k++) {
            size_t i = longest ? len - k : k;
            char saved = tmp[i];
            tmp[i] = '\0';
            int match = fnmatch(pattern, tmp, 0) == 0;
            tmp[i] = saved;
            if (match) {
                char *out = strdup(value + i);
                free(tmp);
                return out;
            }
        }
    } else {
        for (size_t k = 0; k <= len; k++) {
            size_t i = longest ? k : len - k;
            if (fnmatch(pattern, value + i, 0) == 0) {
                tmp[i] = '\0';
                return tmp;
            }
        }
    }
    free(tmp);
    return strdup(value);
}

/* Reports an unset parameter under `set -u`. */
static void check_unset(const char *name, const char *value) {
    if (value == NULL && shell_option('u') && strcmp(name, "@") != 0 && strcmp(name, "*") != 0) {
        fail("%s: parameter not set", name);
    }
}

/* Splits raw text at the first top-level `sep` (outside quotes,
 * parentheses and braces). Returns the index, or -1. */
static long split_at(const char *s, char sep) {
    int depth = 0, quote = 0;
    for (long i = 0; s[i]; i++) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && quote == '"' && s[i + 1]) i++;
            else if (c == quote) quote = 0;
        } else if (c == '\\' && s[i + 1]) {
            i++;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '(' || c == '{') {
            depth++;
        } else if (c == ')' || c == '}') {
            depth--;
        } else if (c == sep && depth == 0) {
            return i;
        }
    }
    return -1;
}

/* An arithmetic operand of ${x:off:len}, given as raw text. */
static long arith_raw(const char *raw) {
    char *s = raw_to_string(raw, 1);
    long v = failed ? 0 : arith_eval(s);
    free(s);
    return v;
}

/* ${x:off} ${x:off:len} (ash/bash): a negative offset or length counts
 * from the end. ${@:off:len} slices the positional parameters. */
static void expand_substring(Buf *b, const char *name, const char *spec, int dq, int *empty_at) {
    char *copy = strdup(spec);
    long colon = split_at(copy, ':');
    if (colon >= 0) copy[colon] = '\0';
    long off = arith_raw(copy);
    int have_len = (colon >= 0);
    long len = have_len ? arith_raw(copy + colon + 1) : 0;
    free(copy);
    if (failed) return;
    /* no string is 2^40 long: clamping keeps the sums below from overflowing */
    const long cap = 1L << 40;
    if (off > cap) off = cap;
    if (off < -cap) off = -cap;
    if (len > cap) len = cap;
    if (len < -cap) len = -cap;

    if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {
        int n = env_positional_count();
        long first = off < 0 ? n + 1 + off : off;   /* $0 is offset 0 */
        if (first < 0) first = 0;
        long last = have_len ? (len < 0 ? n + len : first + len - 1) : n;
        int k = 0;
        for (long i = first; i <= last && i <= n; i++) {
            const char *v = (i == 0) ? NULL : env_positional((int)i);
            char *zero = (i == 0) ? env_param_value("0") : NULL;
            if (k++ > 0) bput(b, ' ', (dq && name[0] == '*') ? A_QUO : A_BREAK);
            if (dq && name[0] == '@') bput(b, ' ', A_KEEP);
            bputs(b, zero ? zero : v, dq ? A_QUO : A_EXP);
            free(zero);
        }
        if (k == 0 && dq && name[0] == '@') *empty_at = 1;
        return;
    }
    char *value = env_param_value(name);
    check_unset(name, value);
    const char *v = value ? value : "";
    long vlen = (long)strlen(v);
    long start = off < 0 ? vlen + off : off;
    if (start < 0) start = 0;
    if (start > vlen) start = vlen;
    long end = have_len ? (len < 0 ? vlen + len : start + len) : vlen;
    if (end > vlen) end = vlen;
    for (long i = start; i < end; i++) bput(b, v[i], dq ? A_QUO : A_EXP);
    free(value);
}

/* ${x/pat/rep} ${x//pat/rep}: replace the first (or every) longest
 * match of pat. ${x/#pat/rep} and ${x/%pat/rep} anchor the match at the
 * start or end (bash; ash takes # and % literally). */
/* How many characters every match of a glob pattern has: -1 when it
 * contains * (any length). `a?[bc]` is 3. */
static long pattern_fixed_length(const char *p) {
    long n = 0;
    while (*p) {
        if (*p == '*') return -1;
        if (*p == '\\' && p[1]) {
            p += 2;
        } else if (*p == '[') {
            const char *q = p + 1;
            if (*q == '!' || *q == '^') q++;
            if (*q == ']') q++;
            while (*q && *q != ']') {
                if (*q == '[' && (q[1] == ':' || q[1] == '.' || q[1] == '=')) {
                    char close = q[1];
                    q += 2;
                    while (*q && !(*q == close && q[1] == ']')) q++;
                    if (*q) q += 2;
                } else {
                    q++;
                }
            }
            p = (*q == ']') ? q + 1 : p + 1;   /* no closing ]: a literal [ */
        } else {
            p++;
        }
        n++;
    }
    return n;
}

static void expand_replace(Buf *b, const char *name, const char *spec, int dq) {
    int all = 0, anchor = 0;
    if (*spec == '/') { all = 1; spec++; }
    else if (*spec == '#' || *spec == '%') { anchor = *spec; spec++; }
    char *copy = strdup(spec);
    long slash = split_at(copy, '/');
    if (slash >= 0) copy[slash] = '\0';
    char *pattern = raw_to_pattern(copy);
    char *rep = slash >= 0 ? raw_to_string(copy + slash + 1, dq) : strdup("");
    free(copy);
    char *value = env_param_value(name);
    check_unset(name, value);
    const char *v = value ? value : "";
    char attr = dq ? A_QUO : A_EXP;
    size_t vlen = strlen(v);
    char *tmp = malloc(vlen + 1);
    size_t i = 0;
    int replaced = 0;
    /* without a *, a match has one possible length: try only that one,
     * so ${x//a/b} on a long value stays linear */
    long fixed = pattern_fixed_length(pattern);
    while (i <= vlen) {
        size_t match = 0;
        int found = 0;
        /* an empty match at the very end, after the text before it was
         * matched, is not another match (replacing all of x with the
         * pattern star gives one copy, not two) */
        if (*pattern && !(replaced && !all) && !(anchor == '#' && i > 0) && !(replaced && i == vlen)) {
            size_t top = vlen - i + 1, low = 0;   /* lengths tried: top-1 down to low */
            if (fixed >= 0) {
                top = ((size_t)fixed <= vlen - i) ? (size_t)fixed + 1 : 0;
                low = (size_t)fixed;
            }
            for (size_t l = top; l-- > low;) {   /* longest first */
                if (anchor == '%' && i + l != vlen) continue;
                memcpy(tmp, v + i, l);
                tmp[l] = '\0';
                if (fnmatch(pattern, tmp, 0) == 0) {
                    match = l;
                    found = 1;
                    break;
                }
            }
        }
        if (found) {
            bputs(b, rep, attr);
            replaced = 1;
            if (match == 0) {
                if (i < vlen) bput(b, v[i], attr);
                i++;
            } else {
                i += match;
            }
            continue;
        }
        if (i < vlen) bput(b, v[i], attr);
        i++;
    }
    free(tmp);
    free(value);
    free(pattern);
    free(rep);
}

/* "${...}: bad substitution", naming the bash feature when it is one:
 * a script written for bash then says what to change. */
static void bad_substitution(const char *inner) {
    const char *p = inner + (*inner == '!' || *inner == '#');
    while (is_name_char(*p)) p++;
    const char *hint = NULL;
    if (*inner == '!') hint = "indirection (${!name}) is a bash feature; use eval: eval \"v=\\$$name\"";
    else if (*p == '[') hint = "arrays are a bash feature; use a space-separated list, or set -- a b c and $1 $2";
    else if (*p == '^' || *p == ',') hint = "case changing is a bash feature; use tr: $(printf %s \"$x\" | tr a-z A-Z)";
    else if (*p == '@') hint = "${name@...} transformations are a bash feature";
    shell_error_prefix();
    if (hint != NULL) fprintf(stderr, "${%s}: bad substitution: %s\n", inner, hint);
    else fprintf(stderr, "${%s}: bad substitution\n", inner);
    failed = 1;
}

/* ${...}: inner is the text between the braces. */
/* Whether the shell's locale (LC_ALL, else LC_CTYPE, else LANG, read
 * from shell variables so a script can change it) uses UTF-8. */
static int utf8_locale(void) {
    const char *names[] = { "LC_ALL", "LC_CTYPE", "LANG" };
    for (int i = 0; i < 3; i++) {
        const char *v = var_get(names[i]);
        if (v == NULL || *v == '\0') continue;
        const char *dot = strchr(v, '.');
        return dot != NULL && (strncasecmp(dot + 1, "UTF-8", 5) == 0 || strncasecmp(dot + 1, "utf8", 4) == 0);
    }
    return 0;
}

/* ${#x}: characters in a UTF-8 locale (as ash), else bytes. */
static size_t text_length(const char *s) {
    if (!utf8_locale()) return strlen(s);
    size_t n = 0;
    for (; *s; s++) {
        if (((unsigned char)*s & 0xc0) != 0x80) n++; /* skip continuation bytes */
    }
    return n;
}

static void expand_braced(Buf *b, const char *inner, int dq, int *empty_at) {
    char attr = dq ? A_QUO : A_EXP;
    const char *p = inner;

    /* ${#name}: length */
    if (p[0] == '#' && p[1] != '\0' && (is_name_start(p[1]) || isdigit((unsigned char)p[1]) ||
                                         (p[2] == '\0' && strchr("@*#?-$!", p[1])))) {
        const char *name = p + 1;
        int valid = 1;
        if (is_name_start(*name)) { for (const char *q = name; *q; q++) if (!is_name_char(*q)) valid = 0; }
        else if (isdigit((unsigned char)*name)) { for (const char *q = name; *q; q++) if (!isdigit((unsigned char)*q)) valid = 0; }
        if (valid) {
            char buf[32];
            if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {
                snprintf(buf, sizeof(buf), "%d", env_positional_count());
            } else {
                char *v = env_param_value(name);
                check_unset(name, v);
                snprintf(buf, sizeof(buf), "%zu", v ? text_length(v) : (size_t)0);
                free(v);
            }
            bputs(b, buf, attr);
            return;
        }
    }

    /* the name */
    size_t n = 0;
    if (is_name_start(p[0])) {
        while (is_name_char(p[n])) n++;
    } else if (isdigit((unsigned char)p[0])) {
        while (isdigit((unsigned char)p[n])) n++;
    } else if (p[0] && strchr("@*#?-$!", p[0])) {
        n = 1;
    }
    if (n == 0) {
        bad_substitution(inner);
        return;
    }
    char *name = strndup(p, n);
    const char *op = p + n;

    if (*op == '\0') {
        if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {
            put_params(b, name[0] == '*', dq, empty_at);
        } else {
            char *v = env_param_value(name);
            check_unset(name, v);
            if (v) bputs(b, v, attr);
            free(v);
        }
        free(name);
        return;
    }

    if (*op == ':' && op[1] != '\0' && strchr("-=?+", op[1]) == NULL) {
        expand_substring(b, name, op + 1, dq, empty_at);
        free(name);
        return;
    }
    if (*op == '/') {
        expand_replace(b, name, op + 1, dq);
        free(name);
        return;
    }

    int colon = (*op == ':');
    char kind = colon ? op[1] : op[0];
    const char *word = op + (colon ? 2 : 1);
    char *value = env_param_value(name);
    int is_set = value != NULL;
    int is_null = !is_set || *value == '\0';

    if (strchr("-=?+", kind) == NULL || (colon && kind == '\0')) {
        if (!colon && (kind == '%' || kind == '#')) {
            int longest = (op[1] == kind);
            char *pattern = raw_to_pattern(op + (longest ? 2 : 1));
            char *out = trim(value ? value : "", pattern, kind == '%', longest);
            check_unset(name, value);
            bputs(b, out, attr);
            free(out);
            free(pattern);
        } else {
            bad_substitution(inner);
        }
        free(value);
        free(name);
        return;
    }

    int use_word = colon ? is_null : !is_set;
    switch (kind) {
    case '-':
        if (use_word) expand_raw(b, word, dq, !dq);
        else bputs(b, value, attr);
        break;
    case '=':
        if (use_word) {
            if (!is_name_start(name[0])) {
                fail("%s: cannot assign in this way", name);
                break;
            }
            char *w = raw_to_string(word, 0);
            if (var_set(name, w) != 0) failed = 1;
            bputs(b, w, attr);
            free(w);
        } else {
            bputs(b, value, attr);
        }
        break;
    case '?':
        if (use_word) {
            char *msg = *word ? raw_to_string(word, 0) : strdup(colon ? "parameter null or not set" : "parameter not set");
            shell_error("%s: %s\n", name, msg);
            free(msg);
            failed = 1;
        } else {
            bputs(b, value, attr);
        }
        break;
    case '+':
        if (!use_word) expand_raw(b, word, dq, !dq);
        break;
    }
    free(value);
    free(name);
}

/* Runs a command substitution's text and appends its output. */
static void put_subst(Buf *b, const char *cmd, int dq) {
    char *out = command_substitute(cmd);
    if (out != NULL) {
        bputs(b, out, dq ? A_QUO : A_EXP);
        free(out);
    }
}

/* Expands the $-expression at w[i] and returns the index after it. */
static size_t expand_dollar(Buf *b, const char *w, size_t i, int dq, int *empty_at) {
    char attr = dq ? A_QUO : A_EXP;
    char next = w[i + 1];
    if (next == '(') {
        size_t close = match_paren(w, i + 1);
        if (w[i + 2] == '(' && close > i + 3 && w[close - 1] == ')' &&
            match_paren(w, i + 2) == close - 1) {
            /* $(( expression )) */
            char *raw = strndup(w + i + 3, close - 1 - (i + 3));
            char *expr = raw_to_string(raw, 1);
            free(raw);
            if (!failed) {
                long v = arith_eval(expr);
                char buf[32];
                snprintf(buf, sizeof(buf), "%ld", v);
                bputs(b, buf, attr);
            }
            free(expr);
        } else {
            char *cmd = strndup(w + i + 2, close - (i + 2));
            put_subst(b, cmd, dq);
            free(cmd);
        }
        return w[close] ? close + 1 : close;
    }
    if (next == '{') {
        size_t close = match_brace(w, i + 1);
        char *inner = strndup(w + i + 2, close - (i + 2));
        expand_braced(b, inner, dq, empty_at);
        free(inner);
        return w[close] ? close + 1 : close;
    }
    char name[256];
    size_t n = 0;
    if (isdigit((unsigned char)next) || (next && strchr("@*#?-$!", next))) {
        name[n++] = next;
    } else if (is_name_start(next)) {
        while (is_name_char(w[i + 1 + n]) && n + 1 < sizeof(name)) {
            name[n] = w[i + 1 + n];
            n++;
        }
    } else {
        bput(b, '$', dq ? A_QUO : A_LIT); /* a lone $ */
        return i + 1;
    }
    name[n] = '\0';
    if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {
        put_params(b, name[0] == '*', dq, empty_at);
    } else {
        char *v = env_param_value(name);
        check_unset(name, v);
        if (v) bputs(b, v, attr);
        free(v);
    }
    return i + 1 + n;
}

/* `...`: the text between backticks, with \$ \` \\ unescaped. */
static size_t expand_backticks(Buf *b, const char *w, size_t i, int dq) {
    size_t j = i + 1;
    char *cmd = malloc(strlen(w) + 1);
    size_t k = 0;
    while (w[j] && w[j] != '`') {
        if (w[j] == '\\' && w[j + 1] && strchr("$`\\", w[j + 1])) j++;
        cmd[k++] = w[j++];
    }
    cmd[k] = '\0';
    put_subst(b, cmd, dq);
    free(cmd);
    return w[j] ? j + 1 : j;
}

/* ~ or ~user at w[i]: returns the index after the prefix, or i if this
 * is not a tilde prefix. */
static size_t expand_tilde(Buf *b, const char *w, size_t i, int assign) {
    size_t j = i + 1;
    while (w[j] && w[j] != '/' && !(assign && w[j] == ':')) {
        if (!(is_name_char(w[j]) || w[j] == '.' || w[j] == '-')) return i;
        j++;
    }
    const char *home = NULL;
    char *user = strndup(w + i + 1, j - i - 1);
    if (*user == '\0') {
        home = var_get("HOME");
    } else {
        struct passwd *pw = getpwnam(user);
        home = pw ? pw->pw_dir : NULL;
    }
    free(user);
    if (home == NULL) return i;
    bputs(b, home, A_QUO);
    return j;
}

/* Expands one marked word into b. lit_as_exp: typed text counts as an
 * expansion result (the word of an unquoted ${x:-word}). assign: tilde
 * prefixes are also recognized after ':' (assignment values). */
static void expand_into_word(Buf *b, const char *w, int lit_as_exp, int assign);

/* ${...} inside ${...} re-reads the inner text at every level, so very
 * deep nesting costs time as well as stack: stop at ACSH_MAX_NESTING
 * (no real script comes near it) or when the stack runs low. */
static void expand_into(Buf *b, const char *w, int lit_as_exp, int assign) {
    static int depth = 0;
    if (depth >= ACSH_MAX_NESTING || stack_exhausted()) {
        if (!failed) fail("%s", "expansion nested too deeply");
        failed = 1;
        return;
    }
    depth++;
    expand_into_word(b, w, lit_as_exp, assign);
    depth--;
}

static void expand_into_word(Buf *b, const char *w, int lit_as_exp, int assign) {
    int sq = 0, dq = 0;
    size_t dq_mark = 0;
    int empty_at = 0;
    for (size_t i = 0; w[i];) {
        char c = w[i];
        if (c == SQ_START) { sq = 1; bput(b, ' ', A_KEEP); i++; continue; }
        if (c == SQ_END)   { sq = 0; i++; continue; }
        if (c == DQ_START) {
            dq = 1;
            dq_mark = b->n;
            empty_at = 0;
            bput(b, ' ', A_KEEP);
            i++;
            continue;
        }
        if (c == DQ_END) {
            dq = 0;
            if (empty_at && b->n == dq_mark + 1) {
                b->n = dq_mark; /* "$@" with no parameters leaves no field */
            }
            i++;
            continue;
        }
        if (sq) {
            bput(b, c, A_QUO);
            i++;
            continue;
        }
        if (c == '$') {
            i = expand_dollar(b, w, i, dq, &empty_at);
            continue;
        }
        if (c == '`') {
            i = expand_backticks(b, w, i, dq);
            continue;
        }
        if ((c == '<' || c == '>') && w[i + 1] == '(' && !dq) {
            /* <(cmd) / >(cmd): a /dev/fd path connected to cmd */
            size_t close = match_paren(w, i + 1);
            char *cmd = strndup(w + i + 2, close - (i + 2));
            int fd = process_substitute(cmd, c == '>');
            free(cmd);
            if (fd < 0) {
                failed = 1;
            } else {
                char path[32];
                snprintf(path, sizeof(path), "/dev/fd/%d", fd);
                bputs(b, path, A_QUO);
            }
            i = w[close] ? close + 1 : close;
            continue;
        }
        if (c == '~' && !dq && (i == 0 || (assign && w[i - 1] == ':'))) {
            size_t after = expand_tilde(b, w, i, assign);
            if (after != i) {
                i = after;
                continue;
            }
        }
        bput(b, c, dq ? A_QUO : (lit_as_exp ? A_EXP : A_LIT));
        i++;
    }
}

/* ---- splitting and globbing ---- */

/* Whether the field can match anything but itself: an unquoted * or ?,
 * or an unquoted [ with a ] after it. A lone `[` (the test command) is
 * not a pattern, and skipping glob() for it saves reading the
 * directory on every `[ ... ]`. */
static int has_glob(const Buf *f) {
    for (size_t i = 0; i < f->n; i++) {
        if (f->a[i] == A_QUO) continue;
        if (f->c[i] == '*' || f->c[i] == '?') return 1;
        if (f->c[i] == '[' && memchr(f->c + i + 1, ']', f->n - i - 1) != NULL) return 1;
    }
    return 0;
}

/* Appends s to a NULL-terminated array that started out NULL with *n 0.
 * The array doubles whenever *n + 1 reaches a power of two, so a
 * million fields cost a few dozen reallocations, not a million. */
static void push_field(char ***out, int *n, char *s) {
    size_t used = (size_t)*n + 1;           /* items plus the NULL */
    if ((used & (used - 1)) == 0) {         /* full: the capacity is a power of two */
        *out = realloc(*out, used * 2 * sizeof(char *));
    }
    if (*out == NULL) {
        shell_error("out of memory\n");
        exit(2);
    }
    (*out)[(*n)++] = s;
    (*out)[*n] = NULL;
}

/* Globs one field (unless set -f or nothing to glob) and adds the
 * result: every match, sorted, or the field itself if none match. */
static void finish_field(Buf *f, char ***out, int *n) {
    if (!shell_option('f') && has_glob(f)) {
        char *pattern = malloc(f->n * 2 + 1);
        size_t k = 0;
        for (size_t i = 0; i < f->n; i++) {
            if (f->a[i] == A_QUO && strchr("*?[]\\", f->c[i])) pattern[k++] = '\\';
            pattern[k++] = f->c[i];
        }
        pattern[k] = '\0';
        glob_t g;
        if (glob(pattern, 0, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) {
                push_field(out, n, strdup(g.gl_pathv[i]));
            }
            globfree(&g);
            free(pattern);
            f->n = 0;
            return;
        }
        free(pattern);
    }
    char *s = malloc(f->n + 1);
    if (f->n > 0) memcpy(s, f->c, f->n); /* f->c is NULL while still empty */
    s[f->n] = '\0';
    push_field(out, n, s);
    f->n = 0;
}

/* Splits an expanded buffer into fields on $IFS (only characters that
 * came from unquoted expansions), then globs each field. */
static void split_fields(const Buf *b, char ***out, int *n) {
    const char *ifs = ifs_value();
    Buf f = { 0 };
    int exists = 0;     /* the current field exists, even if empty */
    for (size_t i = 0; i < b->n; i++) {
        char c = b->c[i], a = b->a[i];
        if (a == A_BREAK) {
            if (exists) finish_field(&f, out, n);
            exists = 0;
            continue;
        }
        if (a == A_KEEP) {
            exists = 1;
            continue;
        }
        if (a == A_EXP && *ifs && strchr(ifs, c)) {
            /* a run of IFS characters: whitespace only separates;
             * each other IFS character ends a field (maybe empty) */
            int others = 0;
            size_t j = i;
            while (j < b->n && b->a[j] == A_EXP && strchr(ifs, b->c[j])) {
                if (!isspace((unsigned char)b->c[j])) others++;
                j++;
            }
            if (others > 0) {
                finish_field(&f, out, n);
                for (int k = 1; k < others; k++) push_field(out, n, strdup(""));
            } else if (exists) {
                finish_field(&f, out, n);
            }
            exists = 0;
            i = j - 1;
            continue;
        }
        bput(&f, c, a);
        exists = 1;
    }
    if (exists) finish_field(&f, out, n);
    bfree(&f);
}

/* ---- brace expansion (bash): {a,b,c}, {1..5}, {a..e}, {01..10..2} ----
 * Done on the marked word before every other expansion. Quoted text,
 * ${...}, $(...) and `...` are left alone, a brace with neither a comma
 * nor a valid sequence stays as it is ({a}, {}, {1..x}), and set +B
 * (set +o braceexpand) turns it off for strict POSIX scripts. */

#define BRACE_MAX_WORDS 4000000   /* {1..4000000}; more is a mistake, not a list */

typedef struct { char **w; int n, cap; } WordList;

static void wl_push(WordList *l, char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->w = realloc(l->w, (size_t)l->cap * sizeof(char *));
    }
    l->w[l->n++] = s;
}

/* Index after the region starting at w[i] that brace expansion must not
 * look into (quotes, $..., `...`), or i if w[i] starts none. */
static size_t skip_region(const char *w, size_t i) {
    char c = w[i];
    if (c == SQ_START || c == DQ_START) {
        char end = (c == SQ_START) ? SQ_END : DQ_END;
        while (w[i] && w[i] != end) i++;
        return w[i] ? i + 1 : i;
    }
    if (c == '$' && (w[i + 1] == '{' || w[i + 1] == '(')) {
        size_t close = (w[i + 1] == '{') ? match_brace(w, i + 1) : match_paren(w, i + 1);
        return w[close] ? close + 1 : close;
    }
    if (c == '`') {
        for (i++; w[i] && w[i] != '`'; i++) if (w[i] == '\\' && w[i + 1]) i++;
        return w[i] ? i + 1 : i;
    }
    return i;
}

/* A sequence {X..Y} or {X..Y..STEP} in text[0, len): adds each item. */
static int brace_sequence(const char *text, size_t len, WordList *items) {
    char buf[64];
    if (len >= sizeof(buf)) return 0;
    memcpy(buf, text, len);
    buf[len] = '\0';
    char *dots = strstr(buf, "..");
    if (dots == NULL) return 0;
    *dots = '\0';
    char *a = buf, *b = dots + 2, *step_s = strstr(b, "..");
    long step = 1;
    if (step_s != NULL) {
        *step_s = '\0';
        char *end;
        step = strtol(step_s + 2, &end, 10);
        if (step_s[2] == '\0' || *end != '\0') return 0;
        if (step < 0) step = -step;
        if (step == 0) step = 1;
    }
    char *ea, *eb;
    long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
    if (*a && *b && *ea == '\0' && *eb == '\0') {
        /* numbers; a leading zero on either end pads all to the same width */
        int width = 0;
        const char *da = (*a == '-') ? a + 1 : a, *db = (*b == '-') ? b + 1 : b;
        if ((da[0] == '0' && da[1]) || (db[0] == '0' && db[1])) {
            width = (int)(strlen(a) > strlen(b) ? strlen(a) : strlen(b));
        }
        long count = (x <= y ? y - x : x - y) / step + 1;
        if (count > BRACE_MAX_WORDS) return -1;
        for (long v = x, k = 0; k < count; k++, v += (x <= y ? step : -step)) {
            char num[48];
            if (width) snprintf(num, sizeof(num), v < 0 ? "-%0*ld" : "%0*ld", v < 0 ? width - 1 : width, v < 0 ? -v : v);
            else snprintf(num, sizeof(num), "%ld", v);
            wl_push(items, strdup(num));
        }
        return 1;
    }
    if (strlen(a) == 1 && strlen(b) == 1 && isalpha((unsigned char)*a) && isalpha((unsigned char)*b)) {
        int from = (unsigned char)*a, to = (unsigned char)*b;
        for (int v = from; from <= to ? v <= to : v >= to; v += (from <= to ? (int)step : -(int)step)) {
            char one[4] = { (char)v, '\0', '\0', '\0' };
            if (v == '\\') { one[0] = SQ_START; one[1] = SQ_END; }  /* bash makes this an empty word */
            else if (strchr("`$\"'", v)) { one[0] = SQ_START; one[1] = (char)v; one[2] = SQ_END; }
            wl_push(items, strdup(one));
        }
        return 1;
    }
    return 0;
}

/* Expands the first brace expression in w (then the results, for the
 * rest) into *out. Returns 0, or -1 when it would make too many words. */
static int brace_expand_into(const char *w, WordList *out, int depth) {
    for (size_t i = 0; w[i];) {
        size_t after = skip_region(w, i);
        if (after != i) { i = after; continue; }
        if (w[i] == '\\' && w[i + 1]) { i += 2; continue; }
        if (w[i] != '{') { i++; continue; }
        /* the matching }, and the commas at this level */
        size_t commas[256];
        int ncommas = 0, level = 0;
        size_t j = i + 1, close = 0;
        while (w[j]) {
            size_t a2 = skip_region(w, j);
            if (a2 != j) { j = a2; continue; }
            if (w[j] == '\\' && w[j + 1]) { j += 2; continue; }
            if (w[j] == '{') level++;
            else if (w[j] == '}') {
                if (level == 0) { close = j; break; }
                level--;
            } else if (w[j] == ',' && level == 0 && ncommas < 256) {
                commas[ncommas++] = j;
            }
            j++;
        }
        if (close == 0) return wl_push(out, strdup(w)), 0;   /* no closing brace: nothing to do */
        WordList items = { 0 };
        if (ncommas > 0) {
            size_t start = i + 1;
            for (int k = 0; k <= ncommas; k++) {
                size_t end = (k < ncommas) ? commas[k] : close;
                wl_push(&items, strndup(w + start, end - start));
                start = end + 1;
            }
        } else {
            int r = brace_sequence(w + i + 1, close - i - 1, &items);
            if (r < 0) { free(items.w); return -1; }
            if (r == 0) { i++; continue; }        /* {a} or {x..y..z}: literal, look further on */
        }
        int status = 0;
        for (int k = 0; k < items.n; k++) {
            size_t pl = i, il = strlen(items.w[k]), sl = strlen(w + close + 1);
            char *joined = malloc(pl + il + sl + 1);
            memcpy(joined, w, pl);
            memcpy(joined + pl, items.w[k], il);
            memcpy(joined + pl + il, w + close + 1, sl + 1);
            if (status == 0 && (out->n > BRACE_MAX_WORDS || depth > 64 ||
                                brace_expand_into(joined, out, depth + 1) != 0)) {
                status = -1;
            }
            free(joined);
            free(items.w[k]);
        }
        free(items.w);
        return status;
    }
    wl_push(out, strdup(w));
    return 0;
}

/* ---- public API ---- */

int expand_fields(char **words, int nwords, char ***out, int *nout) {
    *out = NULL;
    *nout = 0;
    push_field(out, nout, NULL); /* make *out a valid, NULL-terminated array */
    *nout = 0;
    failed = 0;
    for (int i = 0; i < nwords && !failed; i++) {
        WordList braced = { 0 };
        if (shell_option('B') && strchr(words[i], '{') != NULL) {
            if (brace_expand_into(words[i], &braced, 0) != 0) {
                fail("%s", "brace expansion makes too many words");
            }
        } else {
            wl_push(&braced, strdup(words[i]));
        }
        for (int k = 0; k < braced.n; k++) {
            if (!failed) {
                Buf b = { 0 };
                expand_into(&b, braced.w[k], 0, 0);
                if (!failed) split_fields(&b, out, nout);
                bfree(&b);
            }
            free(braced.w[k]);
        }
        free(braced.w);
    }
    if (failed) {
        for (int i = 0; i < *nout; i++) free((*out)[i]);
        (*out)[0] = NULL;
        *nout = 0;
        return -1;
    }
    return 0;
}

char *expand_string(const char *word, int assignment) {
    Buf b = { 0 };
    failed = 0;
    expand_into(&b, word, 0, assignment);
    char *s = bstring(&b);
    bfree(&b);
    if (failed) {
        free(s);
        return NULL;
    }
    return s;
}

char *expand_pattern(const char *word) {
    Buf b = { 0 };
    failed = 0;
    expand_into(&b, word, 0, 0);
    char *p = malloc(b.n * 2 + 1);
    size_t k = 0;
    for (size_t i = 0; i < b.n; i++) {
        if (b.a[i] == A_KEEP || b.a[i] == A_BREAK) continue;
        if (b.a[i] == A_QUO && strchr("*?[]\\", b.c[i])) p[k++] = '\\';
        p[k++] = b.c[i];
    }
    p[k] = '\0';
    bfree(&b);
    if (failed) {
        free(p);
        return NULL;
    }
    return p;
}
