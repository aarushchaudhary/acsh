#define _POSIX_C_SOURCE 200809L
#include <stddef.h>
#include <stdint.h>
#include <sys/resource.h>
#include "acsh.h"

/* One guard against running out of C stack. The parser, the executor,
 * expansion, arithmetic and `test` all recurse on nested input; rather
 * than a separate depth counter in each, they ask stack_exhausted(),
 * which compares how much stack is in use with the real limit. Deep
 * input then gets a clean error ("nested too deeply") instead of a
 * segfault, and ordinary scripts are never limited by an arbitrary
 * number. */

static uintptr_t stack_base;
static size_t    stack_budget;

/* Addresses come from __builtin_frame_address (gcc, clang): the address
 * of a local can be a heap "fake stack" under AddressSanitizer. */
void stack_init(void *base) {
    (void)base;
    stack_base = (uintptr_t)__builtin_frame_address(0);
    size_t limit = 8u << 20;                 /* the usual default */
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < limit) {
        limit = (size_t)rl.rlim_cur;
    }
    /* half: room for libc, signal handlers and sanitizer-sized frames */
    stack_budget = limit / 2;
}

int stack_exhausted(void) {
    if (stack_base == 0) return 0;
    uintptr_t now = (uintptr_t)__builtin_frame_address(0);
    size_t used = (stack_base > now) ? stack_base - now : now - stack_base;
    return used > stack_budget;
}
