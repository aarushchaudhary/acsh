/* libFuzzer target: word expansion and arithmetic. Each input line is
 * one word: it is marked as the lexer would, then expanded into fields,
 * into a string and into a pattern; the first line is also evaluated as
 * arithmetic. Built with -DACSH_FUZZ, so $(...) and <(...) expand to
 * nothing instead of running commands. Build with `make fuzz`. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "acsh.h"
#include "syntax.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static int ready = 0;
    if (!ready) {
        vars_init();
        var_set("x", "a b*c");
        var_set("IFS", " \t\n");
        shell_set_option('f', 1); /* no globbing: a pattern like /x/x/x with stars would walk the disk */
        ready = 1;
    }
    char base;
    stack_init(&base);
    char *text = malloc(size + 1);
    if (text == NULL) return 0;
    memcpy(text, data, size);
    text[size] = '\0';
    long value;
    arith_evaluate(text, &value);
    for (char *line = text, *next; line != NULL; line = next) {
        next = strchr(line, '\n');
        if (next != NULL) *next++ = '\0';
        char *marked = lex_mark(line, 0);
        if (marked == NULL) continue;
        char **fields = NULL;
        int n = 0;
        char *words[1] = { marked };
        if (expand_fields(words, 1, &fields, &n) == 0 && fields != NULL) {
            for (int i = 0; i < n; i++) free(fields[i]);
        }
        free(fields);
        free(expand_string(marked, 1));
        free(expand_pattern(marked));
        free(marked);
    }
    free(text);
    return 0;
}
