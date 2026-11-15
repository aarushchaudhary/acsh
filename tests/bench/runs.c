/* Times N runs of a command: prints the total in milliseconds.
 *   runs N PROGRAM [ARGS...]      e.g.  runs 1000 ./acsh -c true
 * A clock of its own (clock_gettime), so the benchmark does not depend
 * on date +%N, which some busybox builds lack. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: runs N PROGRAM [ARGS...]\n");
        return 2;
    }
    long n = strtol(argv[1], NULL, 10);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (long i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("runs: fork");
            return 2;
        }
        if (pid == 0) {
            execvp(argv[2], argv + 2);
            _exit(127);
        }
        int status;
        if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "runs: %s failed\n", argv[2]);
            return 1;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    printf("%ld\n", ms);
    return 0;
}
