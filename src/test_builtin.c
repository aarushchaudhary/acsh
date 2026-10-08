#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "acsh.h"

/* Implements the POSIX `test` utility (and its `[ ... ]` synonym)
 * natively, rather than relying on an external /usr/bin/test binary
 * happening to exist on the host -- acsh's if/while/until conditions
 * use this constantly (`if test -f x`, `while test $i -lt 10`), so
 * this is core functionality, not a nicety.
 *
 * Supported forms:
 *   test                       -- false (no arguments)
 *   test STRING                -- true if STRING is non-empty
 *   test ! EXPR                -- negation
 *   test STR1 = STR2            test STR1 != STR2
 *   test N1 -eq N2   -ne  -lt  -le  -gt  -ge   (numeric comparisons)
 *   test -z STRING              -n STRING       (empty / non-empty)
 *   test -f PATH   -d PATH   -e PATH   -r/-w/-x PATH   (file tests)
 *   test EXPR1 -a EXPR2          EXPR1 -o EXPR2  (and / or, POSIX-limited
 *                                                  to this simple 2-operand
 *                                                  form -- see note below)
 *
 * Scope note: full POSIX test/[ has precedence and grouping rules for
 * combining -a/-o/! across many operands that even real shells warn
 * are ambiguous for more than a few terms. acsh supports the common,
 * unambiguous shapes above; deeply nested combinations (e.g. more
 * than one -a/-o in a single test) are a known limitation. */

static int is_numeric(const char *s) {
    if (*s == '\0') return 0;
    const char *p = s;
    if (*p == '-' || *p == '+') p++;
    if (*p == '\0') return 0;
    while (*p != '\0') {
        if (*p < '0' || *p > '9') return 0;
        p++;
    }
    return 1;
}

/* Evaluates a single primary test (no -a/-o combining) starting at
 * argv[*idx], advancing *idx past whatever it consumed. Returns 1
 * (true) or 0 (false); on a malformed expression, prints an error and
 * sets *error to 1. */
static int eval_primary(int argc, char *argv[], int *idx, int *error) {
    if (*idx >= argc) {
        *error = 1;
        return 0;
    }

    const char *a = argv[*idx];

    if (strcmp(a, "!") == 0) {
        (*idx)++;
        int inner = eval_primary(argc, argv, idx, error);
        return *error ? 0 : !inner;
    }

    /* unary operators: OP ARG */
    if (strcmp(a, "-z") == 0 || strcmp(a, "-n") == 0) {
        if (*idx + 1 >= argc) { *error = 1; return 0; }
        const char *val = argv[*idx + 1];
        *idx += 2;
        int empty = (val[0] == '\0');
        return (a[1] == 'z') ? empty : !empty;
    }
    if (strcmp(a, "-f") == 0 || strcmp(a, "-d") == 0 || strcmp(a, "-e") == 0 ||
        strcmp(a, "-r") == 0 || strcmp(a, "-w") == 0 || strcmp(a, "-x") == 0 ||
        strcmp(a, "-s") == 0) {
        if (*idx + 1 >= argc) { *error = 1; return 0; }
        const char *path = argv[*idx + 1];
    char op = a[1];
    *idx += 2;

    struct stat st;
    int stat_ok = (stat(path, &st) == 0);

    if (op == 'f') return stat_ok && S_ISREG(st.st_mode);
    if (op == 'd') return stat_ok && S_ISDIR(st.st_mode);
    if (op == 'e') return stat_ok;
    if (op == 'r') return access(path, R_OK) == 0;
    if (op == 'w') return access(path, W_OK) == 0;
    if (op == 'x') return access(path, X_OK) == 0;
    if (op == 's') return stat_ok && st.st_size > 0;
    return 0;
        }

        /* binary operators: ARG1 OP ARG2 -- only recognized when the
         * MIDDLE token is one of these exact operators, so a bare string
         * that happens to contain e.g. "=" elsewhere isn't misread */
        if (*idx + 1 < argc) {
            const char *op = argv[*idx + 1];
            int is_binary =
            strcmp(op, "=") == 0 || strcmp(op, "!=") == 0 ||
            strcmp(op, "-eq") == 0 || strcmp(op, "-ne") == 0 ||
            strcmp(op, "-lt") == 0 || strcmp(op, "-le") == 0 ||
            strcmp(op, "-gt") == 0 || strcmp(op, "-ge") == 0;
            if (is_binary && *idx + 2 < argc) {
                const char *lhs = argv[*idx];
                const char *rhs = argv[*idx + 2];
                *idx += 3;

                if (strcmp(op, "=") == 0) return strcmp(lhs, rhs) == 0;
                if (strcmp(op, "!=") == 0) return strcmp(lhs, rhs) != 0;

                /* numeric comparisons: both sides must look numeric,
                 * otherwise this is a usage error (matches POSIX) */
                if (!is_numeric(lhs) || !is_numeric(rhs)) {
                    fprintf(stderr, "acsh: test: %s: expected a numeric argument\n",
                            is_numeric(lhs) ? rhs : lhs);
                    *error = 1;
                    return 0;
                }
                long l = atol(lhs);
                long r = atol(rhs);
                if (strcmp(op, "-eq") == 0) return l == r;
                if (strcmp(op, "-ne") == 0) return l != r;
                if (strcmp(op, "-lt") == 0) return l < r;
                if (strcmp(op, "-le") == 0) return l <= r;
                if (strcmp(op, "-gt") == 0) return l > r;
                if (strcmp(op, "-ge") == 0) return l >= r;
            }
        }

        /* fallback: a single string argument is true iff non-empty */
        (*idx)++;
        return a[0] != '\0';
}

/* Top-level evaluator: one primary, optionally combined with -a/-o
 * with a SECOND primary (the documented scope limit above). */
static int eval_test(int argc, char *argv[], int *error) {
    int idx = 0;
    int left = eval_primary(argc, argv, &idx, error);
    if (*error) return 0;

    if (idx < argc && (strcmp(argv[idx], "-a") == 0 || strcmp(argv[idx], "-o") == 0)) {
        int is_and = (strcmp(argv[idx], "-a") == 0);
        idx++;
        int right = eval_primary(argc, argv, &idx, error);
        if (*error) return 0;
        return is_and ? (left && right) : (left || right);
    }

    if (idx != argc) {
        /* trailing tokens left over -- malformed expression */
        *error = 1;
        return 0;
    }

    return left;
}

int builtin_test(Command *cmd) {
    int argc = cmd->argc;
    char **argv = cmd->argv;

    /* strip argv[0] ("test", "[", or "[["), and for "[" / "[[" require
     * and strip a matching trailing "]" / "]]" */
    int start = 1;
    int end = argc;

    if (strcmp(argv[0], "[") == 0) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            fprintf(stderr, "acsh: [: missing closing ']'\n");
            return 2;
        }
        end = argc - 1;
    } else if (strcmp(argv[0], "[[") == 0) {
        /* [[ ... ]] is bash/ksh syntax, not POSIX test -- acsh
         * accepts it as a synonym for test/[ using the SAME operator
         * set (see eval_primary() above), plus normalizing == to =
         * below, since that's the one [[ ]]-specific spelling people
         * reach for constantly. Bash's other [[ ]]-only features
         * (=~ regex matching, unquoted word-splitting-safe variable
         * expansion, pattern matching against glob-style patterns in
         * ==) are NOT implemented -- a documented scope limitation,
         * same spirit as test's own -a/-o limitation noted above. */
        if (argc < 2 || strcmp(argv[argc - 1], "]]") != 0) {
            fprintf(stderr, "acsh: [[: missing closing ']]'\n");
            return 2;
        }
        end = argc - 1;
    }

    int inner_argc = end - start;
    if (inner_argc == 0) {
        return 1; /* `test` with no arguments is false */
    }

    /* Build a LOCAL copy of the argument pointer array with ==
     * normalized to = , rather than overwriting argv[] in place:
     * argv[] entries are heap pointers owned by the Pipeline this
     * Command came from (allocated in parser.c, freed later by
     * pipeline_free()) -- replacing one with a pointer to a string
     * literal here would make that later free() call operate on a
     * literal's address instead of a real heap allocation, which is
     * undefined behaviour (and reliably crashes under glibc, as
     * caught during testing: "munmap_chunk(): invalid pointer"). A
     * local array of pointers avoids touching argv[] at all. */
    char *normalized[64];
    int norm_count = inner_argc < 64 ? inner_argc : 64;
    for (int i = 0; i < norm_count; i++) {
        normalized[i] = (strcmp(argv[start + i], "==") == 0)
        ? (char *)"=" : argv[start + i];
    }

    int error = 0;
    int result = eval_test(norm_count, normalized, &error);
    if (error) {
        fprintf(stderr, "acsh: test: malformed expression:");
        for (int i = start; i < end; i++) {
            fprintf(stderr, " %s", argv[i]);
        }
        fprintf(stderr, "\n"
        "  acsh: supported forms: test STR | test STR1 = STR2 | test N1 -eq N2\n"
        "  acsh:                  test -z STR | test -f/-d/-e PATH | test ! EXPR\n");
        return 2;
    }

    return result ? 0 : 1;
}
