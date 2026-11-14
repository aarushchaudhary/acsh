#ifndef ACSH_H
#define ACSH_H

#include <sys/types.h>  /* pid_t */

/* acsh: a POSIX-style shell aiming to be a lighter, friendlier ash.
 *
 * Module map:
 *   main.c       startup, the prompt loop, running script files
 *   lexer.c      input lines -> tokens          (see syntax.h)
 *   parse.c      tokens -> syntax tree          (see syntax.h)
 *   exec.c       runs the syntax tree: pipelines, jobs, redirections,
 *                if/while/until/for/case, break/continue/return
 *   builtins.c   cd, export, read, trap, ...    test_builtin.c: test / [ / [[
 *   functions.c  shell functions
 *   env.c        $VAR / $? / $1 expansion, positional parameters, locals
 *   glob.c       pathname expansion             subst.c: $(...) and `...`
 *   jobs.c       the job table                  signals.c: SIGCHLD/SIGINT/SIGTSTP, traps
 *   alias.c      aliases                        history.c: command history */

#define ACSH_MAX_ALIASES   32     /* aliases stored */
#define ACSH_MAX_JOBS      32     /* background/stopped jobs tracked at once */
#define ACSH_MAX_FUNCTIONS 64     /* shell functions defined at once */
#define ACSH_MAX_CALL_DEPTH 64    /* nested function calls (recursion limit) */

/* ---- builtins.c ---- */

/* A builtin's arguments: argv[0] is the builtin's name, argv[argc] is
 * NULL. Redirections were already applied by the executor. */
typedef struct {
    char **argv;
    int    argc;
    int    argv_cap;
} Command;

/* Runs cmd if it names a builtin (returns 1, status in *exit_status);
 * otherwise returns 0 and does nothing. */
int try_run_builtin(Command *cmd, int *exit_status);

/* 1 if `name` is a builtin (without running it). */
int is_builtin_name(const char *name);

/* ---- test_builtin.c: test / [ / [[ ---- */
int builtin_test(Command *cmd);

/* ---- exec.c ---- */

/* Runs `text` as shell commands in this shell (eval, traps, $(...)).
 * Returns the last command's status, or 2 on a syntax error. */
int run_string(const char *text);

/* Runs an external command (no function or builtin lookup) in the
 * foreground and returns its status. Used by the `command` builtin. */
int exec_external(char **argv);

/* Converts a raw wait() status into a shell exit code: the exit value,
 * or 128 + the signal number for a process killed by a signal. */
int status_to_exit_code(int status);

/* Setup for a forked child that keeps running shell code (a subshell,
 * a pipeline stage, $(...)): default signals, no job control, not
 * interactive, no inherited traps. */
void child_setup_subshell(void);

/* break / continue / return. The builtins raise a signal; the loops and
 * function calls in exec.c and functions.c consume it. */
void control_flow_signal_loop(int is_continue, int level);
int  control_flow_in_loop_depth(void);
void control_flow_signal_return(int status);
int  control_flow_take_return(int *status);  /* 1 + *status if a return was pending */

/* 1 while a break, continue or return is unwinding, or after Ctrl+C:
 * no further command may run until whatever it targets takes it. */
int  control_flow_unwinding(void);

/* Drops any pending break/continue/return (between top-level commands). */
void control_flow_reset(void);

/* ---- jobs.c ---- */

typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} JobState;

/* One job: a pipeline (or forked list) started with & or stopped with
 * Ctrl+Z, tracked by its process group. */
typedef struct {
    int      id;          /* job number shown to the user, e.g. [1] */
    pid_t    pgid;        /* process group of the whole job */
    pid_t   *pids;        /* its processes (heap) */
    int      num_pids;
    JobState state;
    char    *cmdline;     /* its source text, for `jobs` (heap) */
    int      in_use;      /* 0 = free slot */
    int      notify;      /* state changed; not yet reported */
    int      remaining;   /* processes not yet exited */
    int      last_status; /* exit code of the last process, once it exits */
} Job;

/* Registers a job; returns its id, or -1 if the table is full. */
int  jobs_add(pid_t pgid, pid_t pids[], int num_pids, const char *cmdline);

/* Process `pid` of some job exited with raw wait status `status`. When
 * the job's last process is gone the job is Done, with the exit code of
 * its last process. Safe in the SIGCHLD handler: the "[n]+ Done" notice
 * is printed later by jobs_report_changes(). */
void jobs_child_exited(pid_t pid, int status);

/* Process `pid` was stopped (Ctrl+Z): its job is Stopped. Also safe in
 * the handler. */
void jobs_child_stopped(pid_t pid);

void jobs_mark_stopped(pid_t pgid);
void jobs_mark_running(pid_t pgid);

/* Frees a job's slot without a notice (e.g. after `fg` waited for it). */
void jobs_remove(Job *j);

/* Prints the Done/Stopped notices recorded since the last call (only
 * when interactive, as in ash) and frees finished jobs' slots. */
void jobs_report_changes(void);

/* Frees finished jobs (after `jobs` has shown them). */
void jobs_cleanup_done(void);

Job *jobs_find_by_id(int id);      /* job "%id", or NULL */
Job *jobs_find_most_recent(void);  /* newest job, or NULL */
void jobs_print_all(void);         /* the `jobs` listing */

/* ---- signals.c ---- */

/* Installs the shell's SIGCHLD, SIGINT and SIGTSTP handlers. */
void signals_init(void);

/* The process group Ctrl+C / Ctrl+Z are forwarded to (0: none). */
void set_foreground_pgid(pid_t pgid);

/* SIGCHLD is blocked while the shell forks and waits for a foreground
 * job, so the async handler cannot reap a child and lose its status. */
void block_sigchld(void);
void unblock_sigchld(void);

/* Traps: acsh supports INT and EXIT. Trap commands never run inside a
 * signal handler: SIGINT only queues the trap, and it runs between
 * commands (trap_check_and_run_int). */
void trap_set_int(const char *command);
void trap_set_exit(const char *command);
int  trap_check_and_run_int(void);  /* 1 if a queued INT trap ran */
void trap_run_exit(void);           /* once, as the shell exits */

/* Ctrl+C: with no INT trap, SIGINT that reaches the shell (or kills the
 * foreground job) marks the shell interrupted, and running loops and
 * lists stop. Cleared at the next prompt. */
int  shell_interrupted(void);
void shell_clear_interrupted(void);
void shell_note_sigint(void);

/* ---- env.c: variables and expansion ----
 *
 * Variables live in the process environment (setenv), so every
 * variable is visible to commands acsh runs. */

/* Expands $VAR, ${VAR}, $?, $1..$9, $#, $@, $(...) and `...` in a
 * marked word (see syntax.h), plus a leading ~. Quoted regions stay
 * marked so glob_expand() leaves them alone. Returns a heap string. */
char *env_expand(const char *word);

void env_set_last_status(int status);
int  env_get_last_status(void);

/* Positional parameters of the innermost function call. */
void env_push_positional_params(int argc, char *argv[]);
void env_pop_positional_params(void);
int  env_shift_positional_params(int n);
int  env_set_positional_params(int argc, char *argv[]);  /* set -- ARGS */
int  env_positional_count(void);
const char *env_positional(int i);  /* $i, 1-based; "" if unset */

/* `local` variables: a frame per function call, restored on return. */
void env_push_locals_frame(void);
void env_pop_locals_frame(void);
int  env_declare_local(const char *name, const char *value);

/* How many function calls are nested (for the recursion limit). */
int env_get_call_depth(void);

/* ---- glob.c: pathname expansion (*, ?, [abc]) ----
 *
 * Expands an expanded word into a new heap array of strings (*results)
 * and returns how many: every match, sorted, or the word itself (quote
 * markers removed) when it has no unquoted glob characters or nothing
 * matches. The caller frees each string and the array. */
int glob_expand(const char *word, char ***results);

/* ---- subst.c: $(...) and `...` ----
 *
 * Runs cmd in a forked subshell and returns its output (heap) without
 * trailing newlines. Sets $? to the subshell's status. */
char *command_substitute(const char *cmd);

/* ---- functions.c ---- */

int  function_is_defined(const char *name);
void function_undefine(const char *name);

/* Calls function argv[0] with argv[1..] as $1...; returns its status. */
int  function_call(int argc, char *argv[]);

/* ---- alias.c ----
 *
 * acsh ships default aliases (ll, la, .., ...) for interactive shells;
 * ~/.acshrc can override them. An alias is expanded once (an alias
 * that starts with its own name does not loop). */

typedef struct {
    char name[64];
    char value[256];
    int  in_use;
} Alias;

void alias_load_defaults(void);
void alias_set(const char *name, const char *value);
void alias_unset(const char *name);
const char *alias_get(const char *name);
void alias_print_all(void);

/* ---- history.c ----
 *
 * The last ACSH_HISTORY_SIZE commands, kept in ~/.acsh_history (mode
 * 0600) and appended as they are typed. */

#define ACSH_HISTORY_SIZE 200

void history_load(void);
void history_add(const char *line);   /* skips blanks and repeats */
void history_save_append(void);       /* appends the newest entry to the file */
void history_print_all(void);

/* ---- main.c ---- */

/* Runs a script file in this shell (scripts, ~/.acshrc, `source`).
 * Returns the last command's status, or -1 if it cannot be opened. */
int run_script_file(const char *path);

/* 1 in the interactive shell; 0 in scripts and subshells. */
int  shell_is_interactive(void);
void shell_mark_subshell(void);

#endif /* ACSH_H */
