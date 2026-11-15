/* libFuzzer target: the lexer and parser. Any input is read as a script
 * and parsed to the end; nothing is run. Build with `make fuzz`. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "acsh.h"
#include "syntax.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char base;
    stack_init(&base);
    char *text = malloc(size + 1);
    if (text == NULL) return 0;
    memcpy(text, data, size);
    text[size] = '\0';
    Lexer *lx = lexer_new(text, NULL, NULL, "fuzz");
    /* a syntax error drops the rest of its line and parsing goes on;
     * the cap only guards the harness against a parser that stops
     * making progress (which would itself be a bug worth finding) */
    for (int i = 0; i < 1000000; i++) {
        Node *n = NULL;
        ParseResult r = parse_next(lx, &n);
        node_unref(n);
        if (r == PARSE_EOF) break;
    }
    lexer_free(lx);
    free(text);
    return 0;
}
