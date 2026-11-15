#define _DEFAULT_SOURCE           /* SIGWINCH and friends under -std=c11 */
#define _POSIX_C_SOURCE 200809L  /* sigaction, kill, getpgid under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include "acsh.h"

/* ---- signal names (kill, trap) ---- */

static const struct { const char *name; int num; } SIGNALS[] = {
    { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "ILL", SIGILL },
    { "TRAP", SIGTRAP }, { "ABRT", SIGABRT }, { "BUS", SIGBUS }, { "FPE", SIGFPE },
    { "KILL", SIGKILL }, { "USR1", SIGUSR1 }, { "SEGV", SIGSEGV }, { "USR2", SIGUSR2 },
    { "PIPE", SIGPIPE }, { "ALRM", SIGALRM }, { "TERM", SIGTERM },
#ifdef SIGSTKFLT
    { "STKFLT", SIGSTKFLT },
#endif
    { "CHLD", SIGCHLD }, { "CONT", SIGCONT }, { "STOP", SIGSTOP }, { "TSTP", SIGTSTP },
    { "TTIN", SIGTTIN }, { "TTOU", SIGTTOU }, { "URG", SIGURG }, { "XCPU", SIGXCPU },
    { "XFSZ", SIGXFSZ }, { "VTALRM", SIGVTALRM }, { "PROF", SIGPROF }, { "WINCH", SIGWINCH },
#ifdef SIGIO
    { "IO", SIGIO },
#endif
#ifdef SIGPWR
    { "PWR", SIGPWR },
#endif
    { "SYS", SIGSYS },
    { NULL, 0 }
};

int signal_number(const char *s) {
    if (isdigit((unsigned char)*s)) {
        char *end;
        long n = strtol(s, &end, 10);
        return (*end == '\0' && n >= 0 && n < NSIG) ? (int)n : -1;
    }
    if (strncasecmp(s, "SIG", 3) == 0) s += 3;
    for (int i = 0; SIGNALS[i].name != NULL; i++) {
        if (strcasecmp(s, SIGNALS[i].name) == 0) return SIGNALS[i].num;
    }
    return -1;
}

const char *signal_name(int num) {
    for (int i = 0; SIGNALS[i].name != NULL; i++) {
        if (SIGNALS[i].num == num) return SIGNALS[i].name;
    }
    return NULL;
}

void signal_list(void) {
    for (int n = 1; n < NSIG; n++) {
        const char *name = signal_name(n);
        if (name != NULL) printf("%2d) %s\n", n, name);
    }
}

/* The shell needs to know which process group is currently running in
 * the foreground so that Ctrl+C/Ctrl+Z can be routed to it instead of
 * killing the shell itself. exec.c sets this before waiting on a
 * foreground job, and clears it (back to 0) once it's done. */
static volatile pid_t foreground_pgid = 0;

void set_foreground_pgid(pid_t pgid) {
    foreground_pgid = pgid;
}

/* ---- traps ----
 *
 * trap_cmd[sig] is the action for a signal: NULL = default, "" =
 * ignored, otherwise a command. A trapped signal only marks itself
 * pending in the handler; the command runs between commands
 * (trap_run_pending), never inside the handler, where running shell
 * code is not safe. EXIT runs as the shell exits, ERR when a command
 * fails (under the same rules as set -e). */

static char *trap_cmd[NSIG];
static char *exit_trap = NULL;
static char *err_trap = NULL;
static volatile sig_atomic_t pending[NSIG];
static volatile sig_atomic_t any_pending = 0;

static void trap_handler(int sig) {
    pending[sig] = 1;
    any_pending = 1;
}

/* Signals ignored when a non-interactive shell started (or, in an
 * asynchronous command, SIGINT and SIGQUIT): POSIX says they stay
 * ignored -- trap cannot catch or reset them, and children inherit it. */
static unsigned char locked_ignored[NSIG];
static unsigned char entry_checked[NSIG];
static int noninteractive_at_start = 0;

/* Whether sig was ignored when a non-interactive shell started. Asked
 * the first time it matters, which is always before the shell itself
 * changes sig (at startup for INT and TSTP, in trap for the rest), so
 * a script does not pay a sigaction() for every signal at startup.
 * Subshells of an interactive shell never lock: what they inherit was
 * set by that shell's job control, not by whoever started it. */
static void check_entry(int sig) {
    if (sig <= 0 || sig >= NSIG || entry_checked[sig]) return;
    entry_checked[sig] = 1;
    struct sigaction old;
    if (noninteractive_at_start && sig != SIGKILL && sig != SIGSTOP && sig != SIGCHLD &&
        sigaction(sig, NULL, &old) == 0 && old.sa_handler == SIG_IGN) {
        locked_ignored[sig] = 1;
    }
}

void signal_lock_ignored(int sig) {
    entry_checked[sig] = 1;
    locked_ignored[sig] = 1;
    signal(sig, SIG_IGN);
}

void signal_default(int sig) {
    check_entry(sig);
    signal(sig, locked_ignored[sig] ? SIG_IGN : SIG_DFL);
}

/* The handler a signal gets when it has no user trap. */
static void default_disposition(int sig) {
    if (sig == SIGINT || sig == SIGCHLD || sig == SIGTSTP) {
        return; /* the shell's own handlers stay; they consult trap_cmd */
    }
    if (job_control_enabled() &&
        (sig == SIGTTOU || sig == SIGTTIN || sig == SIGQUIT || sig == SIGTERM)) {
        signal(sig, SIG_IGN); /* an interactive shell ignores these */
        return;
    }
    signal_default(sig);
}

int trap_is_ignored(int sig) {
    return sig > 0 && sig < NSIG && trap_cmd[sig] != NULL && *trap_cmd[sig] == '\0';
}

static void install_shell_handler(int sig);

int trap_set(const char *spec, const char *action) {
    if (strcmp(spec, "EXIT") == 0 || strcmp(spec, "0") == 0) {
        free(exit_trap);
        exit_trap = action ? strdup(action) : NULL;
        return 0;
    }
    if (strcmp(spec, "ERR") == 0) {
        free(err_trap);
        err_trap = action ? strdup(action) : NULL;
        return 0;
    }
    int sig = signal_number(spec);
    if (sig <= 0) {
        return -1;
    }
    check_entry(sig);
    if (locked_ignored[sig]) {
        return 0; /* ignored on entry: silently stays ignored (POSIX) */
    }
    free(trap_cmd[sig]);
    trap_cmd[sig] = action ? strdup(action) : NULL;
    if (sig == SIGKILL || sig == SIGSTOP) {
        return 0; /* cannot be caught or ignored; nothing to install */
    }
    if (sig == SIGINT || sig == SIGTSTP) {
        /* trap '' really ignores it, so commands inherit that; otherwise
         * the shell's own handler runs (and consults trap_cmd) */
        if (action != NULL && *action == '\0') signal(sig, SIG_IGN);
        else install_shell_handler(sig);
        return 0;
    }
    if (sig == SIGCHLD) {
        return 0; /* the shell's own handler stays; it consults trap_cmd */
    }
    if (action == NULL) {
        default_disposition(sig);
    } else if (*action == '\0') {
        signal(sig, SIG_IGN);
    } else {
        struct sigaction sa;
        sa.sa_handler = trap_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; /* no SA_RESTART: a trapped signal interrupts `wait` */
        sigaction(sig, &sa, NULL);
    }
    return 0;
}

int trap_pending_signal(void) {
    if (!any_pending) return 0;
    for (int sig = 1; sig < NSIG; sig++) {
        if (pending[sig]) return sig;
    }
    return 0;
}

/* While a trap action runs: the status from just before it. POSIX: a
 * bare `exit` in a trap action exits with that status, not with the
 * status of the trap's own last command (autoconf's EXIT trap ends in
 * `exit $exit_status` with exit_status empty). */
static int in_trap = 0;
static int status_before_trap = 0;

int trap_exit_status(int fallback) {
    return in_trap ? status_before_trap : fallback;
}

static void run_in_trap(const char *cmd) {
    int saved = env_get_last_status();
    int outer_in = in_trap, outer_status = status_before_trap;
    in_trap = 1;
    status_before_trap = saved;
    run_string(cmd);
    in_trap = outer_in;
    status_before_trap = outer_status;
    env_set_last_status(saved);  /* a trap does not change $? */
}

static void run_trap_command(const char *cmd) {
    char *copy = strdup(cmd);   /* the trap may reset itself */
    if (copy != NULL) run_in_trap(copy);
    free(copy);
}

int trap_run_pending(void) {
    if (!any_pending) {
        return 0;
    }
    any_pending = 0;
    int ran = 0;
    for (int sig = 1; sig < NSIG; sig++) {
        if (!pending[sig]) continue;
        pending[sig] = 0;
        if (trap_cmd[sig] != NULL && *trap_cmd[sig] != '\0') {
            run_trap_command(trap_cmd[sig]);
            ran = 1;
        }
    }
    return ran;
}

/* The EXIT trap. It sees the exit status in $? and the shell still
 * exits with that status afterwards, unless the trap itself calls exit
 * (configure's trap 'exit_status=$?; ...; exit $exit_status' 0). */
void trap_run_exit(void) {
    if (exit_trap != NULL) {
        /* cleared first, so an `exit` inside the trap cannot run it again */
        char *cmd = exit_trap;
        exit_trap = NULL;
        run_in_trap(cmd);
        free(cmd);
    }
}

void trap_run_err(void) {
    if (err_trap != NULL && *err_trap != '\0') {
        run_trap_command(err_trap);
    }
}

void trap_reset_for_subshell(void) {
    /* POSIX: a subshell keeps ignored signals ignored; traps with a
     * command go back to the default */
    for (int sig = 1; sig < NSIG; sig++) {
        if (trap_cmd[sig] != NULL && *trap_cmd[sig] != '\0') {
            free(trap_cmd[sig]);
            trap_cmd[sig] = NULL;
            default_disposition(sig);
        }
        pending[sig] = 0;
    }
    any_pending = 0;
    free(exit_trap);
    exit_trap = NULL;
    free(err_trap);
    err_trap = NULL;
}

static void print_trap(const char *cmd, const char *name) {
    fputs("trap -- ", stdout);
    print_single_quoted(cmd);
    printf(" %s\n", name);
}

void trap_print(void) {
    if (exit_trap != NULL) print_trap(exit_trap, "EXIT");
    for (int sig = 1; sig < NSIG; sig++) {
        if (trap_cmd[sig] != NULL && signal_name(sig) != NULL) print_trap(trap_cmd[sig], signal_name(sig));
    }
    if (err_trap != NULL) print_trap(err_trap, "ERR");
}

int trap_print_one(const char *spec) {
    if (strcmp(spec, "EXIT") == 0 || strcmp(spec, "0") == 0) {
        if (exit_trap != NULL) print_trap(exit_trap, "EXIT");
        return 0;
    }
    if (strcmp(spec, "ERR") == 0) {
        if (err_trap != NULL) print_trap(err_trap, "ERR");
        return 0;
    }
    int sig = signal_number(spec);
    if (sig <= 0) return -1;
    if (trap_cmd[sig] != NULL) print_trap(trap_cmd[sig], signal_name(sig));
    return 0;
}

/* Blocks/unblocks SIGCHLD around the shell's own fork()/waitpid()
 * pairs (run_job() in exec.c, command_substitute(), fg). Without it the
 * async sigchld_handler() below can reap a child first, the explicit
 * waitpid() then fails with ECHILD, and the real exit status is lost
 * (`true | false && echo x` used to run the echo). Bash uses the same
 * technique for this hazard. */
void block_sigchld(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_BLOCK, &set, NULL);
}

void unblock_sigchld(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
}

/* SIGCHLD fires whenever ANY child changes state (exits, is killed, or
 * is stopped). We reap every child we can with a non-blocking
 * waitpid(-1, ..., WNOHANG) loop so zombies never pile up, and update
 * the job table for any pgid that finished or stopped in the
 * background while we weren't explicitly waiting on it. */
static void sigchld_handler(int signo) {
    (void)signo;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        /* Jobs are found by pid, not getpgid(): a reaped process no
         * longer has a process group to look up. */
        if (WIFSTOPPED(status)) {
            jobs_child_stopped(pid);
        } else {
            jobs_child_exited(pid, status);
        }
    }
}

/* Ctrl+C: forward SIGINT to the foreground job's process group instead
 * of letting it kill the shell itself. If nothing is in the
 * foreground (we're at the prompt) and a `trap ... INT` is set, raise
 * the pending-trap flag for main.c to act on between commands, since
 * running the trap command directly here would not be async-signal-
 * safe. With no trap set and nothing in the foreground, do nothing,
 * matching how bash behaves at an empty prompt. */
static volatile sig_atomic_t interrupted = 0;

int shell_interrupted(void) {
    return interrupted;
}

void shell_clear_interrupted(void) {
    interrupted = 0;
}

/* A Ctrl+C reached the shell: with an INT trap set, queue the trap
 * (run between commands; execution then goes on, as in ash); without
 * one, mark the shell interrupted so running loops and lists stop.
 * Only touches sig_atomic_t flags, so it is safe in the handler. */
void shell_note_sigint(void) {
    const char *t = trap_cmd[SIGINT];
    if (t == NULL) {
        interrupted = 1;
    } else if (*t != '\0') {
        pending[SIGINT] = 1;
        any_pending = 1;
    }
    /* trap '' INT: ignored */
}

static void sigint_handler(int signo) {
    if (foreground_pgid > 0) {
        /* the job gets it; if it dies of it, the executor calls
         * shell_note_sigint() so an enclosing loop reacts too */
        kill(-foreground_pgid, signo);
        return;
    }
    shell_note_sigint();
}

/* Ctrl+Z: same idea, forward SIGTSTP to the foreground job. */
static void sigtstp_handler(int signo) {
    if (foreground_pgid > 0) {
        kill(-foreground_pgid, signo);
    }
}

static void install_shell_handler(int sig) {
    struct sigaction sa;
    sa.sa_handler = (sig == SIGINT) ? sigint_handler : sigtstp_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(sig, &sa, NULL);
}

void signals_init(void) {
    noninteractive_at_start = !shell_is_interactive();
    check_entry(SIGINT);   /* the two the shell changes right away */
    check_entry(SIGTSTP);
    struct sigaction sa_chld;
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_int;
    sa_int.sa_handler = sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = SA_RESTART;
    if (!locked_ignored[SIGINT]) sigaction(SIGINT, &sa_int, NULL);

    struct sigaction sa_tstp;
    sa_tstp.sa_handler = sigtstp_handler;
    sigemptyset(&sa_tstp.sa_mask);
    sa_tstp.sa_flags = SA_RESTART;
    if (!locked_ignored[SIGTSTP]) sigaction(SIGTSTP, &sa_tstp, NULL);
}
