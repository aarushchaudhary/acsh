#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "acsh.h"
#include "syntax.h"

/* Shell functions: name() { body }. The body is kept as a syntax tree
 * and shared by reference: a function that redefines itself while it
 * runs keeps running its old body until it returns. */

typedef struct {
    char *name;
    Node *body;
} ShellFunction;

/* The table grows as needed. function_call() takes its own reference
 * to the body first, so the table may move while a function runs. */
static ShellFunction *functions = NULL;
static int            nfunctions = 0, cap = 0;

static ShellFunction *find_function(const char *name) {
    for (int i = 0; i < nfunctions; i++) {
        if (strcmp(functions[i].name, name) == 0) return &functions[i];
    }
    return NULL;
}

int function_is_defined(const char *name) {
    return find_function(name) != NULL;
}

void function_define(const char *name, Node *body) {
    ShellFunction *fn = find_function(name);
    if (fn != NULL) {
        node_unref(fn->body);
        fn->body = body;
        return;
    }
    if (nfunctions == cap) {
        int ncap = cap ? cap * 2 : 32;
        ShellFunction *bigger = realloc(functions, (size_t)ncap * sizeof(ShellFunction));
        if (bigger == NULL) {
            shell_error("out of memory, cannot define '%s'\n", name);
            node_unref(body);
            return;
        }
        functions = bigger;
        cap = ncap;
    }
    functions[nfunctions].name = strdup(name);
    if (functions[nfunctions].name == NULL) {
        shell_error("out of memory, cannot define '%s'\n", name);
        node_unref(body);
        return;
    }
    functions[nfunctions++].body = body;
}

void function_undefine(const char *name) {
    ShellFunction *fn = find_function(name);
    if (fn == NULL) return;
    int i = (int)(fn - functions);
    node_unref(fn->body);
    free(fn->name);
    memmove(functions + i, functions + i + 1, (size_t)(nfunctions - i - 1) * sizeof(ShellFunction));
    nfunctions--;
}

int function_call(int argc, char *argv[]) {
    ShellFunction *fn = find_function(argv[0]);
    if (fn == NULL) {
        shell_error("%s: function not found\n", argv[0]);
        return 127;
    }
    /* refuse runaway recursion before it exhausts the C stack */
    if (env_get_call_depth() >= ACSH_MAX_CALL_DEPTH) {
        shell_error("%s: maximum function nesting depth (%d) exceeded\n",
                argv[0], ACSH_MAX_CALL_DEPTH);
        return 1;
    }

    Node *body = node_ref(fn->body);
    env_push_positional_params(argc - 1, argv + 1);
    env_push_locals_frame();

    int status = exec_node(body);
    int returned;
    if (control_flow_take_return(&returned)) {
        status = returned;
    }

    env_pop_locals_frame();
    env_pop_positional_params();
    node_unref(body);
    return status;
}

/* Every function name, for Tab completion. */
void function_each(void (*fn)(const char *name, void *ctx), void *ctx) {
    for (int i = 0; i < nfunctions; i++) fn(functions[i].name, ctx);
}
