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
    char  name[64];
    Node *body;
    int   in_use;
} ShellFunction;

static ShellFunction function_table[ACSH_MAX_FUNCTIONS];

static ShellFunction *find_function(const char *name) {
    for (int i = 0; i < ACSH_MAX_FUNCTIONS; i++) {
        if (function_table[i].in_use && strcmp(function_table[i].name, name) == 0) {
            return &function_table[i];
        }
    }
    return NULL;
}

int function_is_defined(const char *name) {
    return find_function(name) != NULL;
}

void function_define(const char *name, Node *body) {
    ShellFunction *slot = find_function(name);
    for (int i = 0; slot == NULL && i < ACSH_MAX_FUNCTIONS; i++) {
        if (!function_table[i].in_use) {
            slot = &function_table[i];
        }
    }
    if (slot == NULL) {
        shell_error("function table full, cannot define '%s'\n", name);
        node_unref(body);
        return;
    }
    if (strlen(name) >= sizeof(slot->name)) {
        shell_error("function name too long: %s\n", name);
        node_unref(body);
        return;
    }
    node_unref(slot->in_use ? slot->body : NULL);
    strcpy(slot->name, name);
    slot->body = body;
    slot->in_use = 1;
}

void function_undefine(const char *name) {
    ShellFunction *fn = find_function(name);
    if (fn != NULL) {
        node_unref(fn->body);
        fn->body = NULL;
        fn->in_use = 0;
    }
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
