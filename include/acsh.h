#ifndef ACSH_H
#define ACSH_H

#include <sys/types.h>  /* pid_t */
#include <termios.h>    /* struct termios (Job) */

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
 *   expand.c     word expansion: $x ${x...} $(...) $((...)), $IFS splitting, globbing
 *   env.c        parameters ($?, $1, $#, ...), positional parameters, locals
 *   subst.c      $(...) and `...`
 *   jobs.c       the job table                  signals.c: SIGCHLD/SIGINT/SIGTSTP, traps
 *   alias.c      aliases                        history.c: command history */

#define ACSH_MAX_CALL_DEPTH 1000  /* nested function calls (recursion limit) */
#define ACSH_MAX_NESTING    256   /* nested ${...} or $(...) inside one another */

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

/* 1 for POSIX special builtins (: . break eval exec exit export ...). */
int is_special_builtin(const char *name);

/* ---- test_builtin.c: test / [ / [[ ;  printf.c: echo, printf ---- */
int builtin_test(Command *cmd);
int builtin_echo(Command *cmd);
int builtin_printf(Command *cmd);

/* ---- exec.c ---- */

/* Runs `text` as shell commands in this shell (eval, traps, $(...)).
 * Returns the last command's status, or 2 on a syntax error. */
int run_string(const char *text);

/* Error messages: "acsh: FILE: line N: <message>" while running a
 * script (or -c), plain "acsh: <message>" at the prompt. fmt includes
 * its own trailing newline. */
void shell_error(const char *fmt, ...);
void shell_error_prefix(void);   /* just the prefix, for messages built in pieces */

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

/* Around reading a file (script, `.`, profile): a `return` outside any
 * function then ends that file instead of the shell. */
void control_flow_enter_file(void);
void control_flow_leave_file(void);
int  control_flow_in_file(void);

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
    int      signaled;    /* the signal that killed the last process, or 0 */
    int      seq;         /* recency, for the current (%+) and previous (%-) job */
    int      has_tmodes;  /* tmodes holds its terminal settings (saved when stopped) */
    int      own_group;   /* started under job control: pgid is a real process group */
    struct termios tmodes;
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

/* A job spec: %n, %+ / %% / % (current), %- (previous), %name (command
 * starts with name), %?text (command contains text). NULL if none. */
Job *jobs_resolve(const char *spec);
Job *jobs_find_by_id(int id);      /* job "%id", or NULL */
Job *jobs_find_by_pid(pid_t pid);  /* the job a process belongs to, or NULL */
int  jobs_capacity(void);           /* slots in the job table (it grows) */
Job *jobs_at(int index);           /* slot 0..jobs_capacity()-1 if in use, else NULL */
void jobs_print_all(int with_pid, int pids_only);  /* jobs, jobs -l, jobs -p */

/* Job control (interactive shells only): on a terminal the shell takes
 * its own process group and the terminal, and ignores SIGTTOU, SIGTTIN,
 * SIGQUIT and SIGTERM; each foreground job gets the terminal while it
 * runs, with its own terminal settings restored on fg. */
void job_control_init(void);
int  job_control_enabled(void);
void terminal_give(Job *j, pid_t pgid);   /* to a foreground job (j may be NULL) */
void terminal_take_back(Job *stopped);    /* back to the shell; saves a stopped job's settings */

/* ---- signals.c ---- */

/* Signal names without "SIG": "TERM" <-> 15. signal_number() also
 * takes "SIGTERM", "term" and "15"; it returns -1 if unknown. */
int         signal_number(const char *name);
const char *signal_name(int num);   /* NULL if unknown */
void        signal_list(void);      /* " 1) HUP" ... (kill -l) */

/* Installs the shell's SIGCHLD, SIGINT and SIGTSTP handlers. */
void signals_init(void);

/* The process group Ctrl+C / Ctrl+Z are forwarded to (0: none). */
void set_foreground_pgid(pid_t pgid);

/* SIGCHLD is blocked while the shell forks and waits for a foreground
 * job, so the async handler cannot reap a child and lose its status. */
void block_sigchld(void);
void unblock_sigchld(void);

/* Traps. A trapped signal is only marked pending by its handler; the
 * command runs between commands (trap_run_pending), never inside the
 * handler. spec: a signal name or number, EXIT/0, or ERR. action: a
 * command, "" to ignore the signal, NULL for the default. */
int  trap_set(const char *spec, const char *action);  /* -1: unknown signal */
int  trap_run_pending(void);      /* runs queued traps; 1 if any ran */
int  trap_pending_signal(void);   /* a queued trapped signal, or 0 */
int  trap_is_ignored(int sig);    /* trap '' SIG */
void trap_run_exit(void);         /* once, as the shell exits */
void trap_run_err(void);          /* after a failing command (rules of set -e) */
void trap_reset_for_subshell(void);
void signal_lock_ignored(int sig); /* ignore sig for good (POSIX entry-ignored rule) */
void signal_default(int sig);      /* SIG_DFL, or SIG_IGN if locked ignored */
void trap_print(void);            /* trap -- 'cmd' NAME ... */
int  trap_print_one(const char *spec); /* trap -p SIG; -1: no such signal */

/* Ctrl+C: with no INT trap, SIGINT that reaches the shell (or kills the
 * foreground job) marks the shell interrupted, and running loops and
 * lists stop. Cleared at the next prompt. */
int  shell_interrupted(void);
void shell_clear_interrupted(void);
void shell_note_sigint(void);

/* ---- vars.c: shell variables ----
 *
 * All variables live in one table. Exported ones (export, set -a, or
 * inherited from the environment) are passed to commands acsh runs;
 * the others stay inside the shell. */

#define V_EXPORT   1
#define V_READONLY 2

void        vars_init(void);                       /* import the environment */
const char *var_get(const char *name);             /* NULL if unset */
int         var_set(const char *name, const char *value);  /* -1: read-only (reported) */
int         var_unset(const char *name);           /* -1: read-only (reported) */
int         var_flags(const char *name);
void        var_add_flags(const char *name, int flags);
void        var_clear_flags(const char *name, int flags);
int         var_valid_name(const char *name);
char      **var_environ(void);                     /* "NAME=value" for exported variables */

/* Saves a variable (value and flags, or that it was unset) so a
 * one-command assignment can be undone with var_restore(). */
typedef struct {
    char *name;
    char *value;
    int   flags;
    int   existed;
} VarSave;
VarSave var_save(const char *name);
void    var_restore(VarSave *s);

/* `set` (all, as name='value'), `export -p`, `readonly -p` listings. */
enum { VARS_ALL, VARS_EXPORTED, VARS_READONLY };
void var_print(int which);

/* ---- env.c: variables and expansion ----
 *
 * Variables live in the process environment (setenv), so every
 * variable is visible to commands acsh runs. */


void env_set_last_status(int status);
int  env_get_last_status(void);

/* Positional parameters of the innermost function call. */
void env_push_positional_params(int argc, char *argv[]);
void env_pop_positional_params(void);
int  env_shift_positional_params(int n);
int  env_set_positional_params(int argc, char *argv[]);  /* set -- ARGS */
int  env_positional_count(void);

/* Startup: $0 = name, $1.. = argv[0..argc-1], $$ = this process. */
void env_init(const char *name, int argc, char *argv[]);
void env_set_last_background(pid_t pid);  /* $! */
void env_seed_random(unsigned long seed);  /* RANDOM=N */
void env_set_lineno(int line);             /* $LINENO */
int  env_get_lineno(void);

/* The value of parameter `name` (a variable, $1.., or one of
 * # ? @ * $ ! - 0) as a heap string, or NULL if it is unset. */
char *env_param_value(const char *name);
const char *env_positional(int i);  /* $i, 1-based; "" if unset */

/* `local` variables: a frame per function call, restored on return. */
void env_push_locals_frame(void);
void env_pop_locals_frame(void);
int  env_declare_local(const char *name, const char *value);

/* How many function calls are nested (for the recursion limit). */
int env_get_call_depth(void);

/* ---- expand.c: word expansion ----
 *
 * Words come from the lexer in marked form (see syntax.h). Expansion
 * runs tilde, parameter, command, arithmetic expansion, then field
 * splitting on $IFS, globbing (unless set -f) and quote removal. On an
 * expansion error (${x?}, $((1/0)), bad substitution) a message is
 * printed and these return -1 / NULL. */

/* Words -> fields: a new NULL-terminated heap array (*out, *n). */
int   expand_fields(char **words, int nwords, char ***out, int *n);

/* One word -> one string, no splitting or globbing (assignment values,
 * redirection targets, case words). assignment: ~ also after ':'. */
char *expand_string(const char *word, int assignment);

/* One word -> an fnmatch() pattern; quoted characters match literally. */
char *expand_pattern(const char *word);

/* Evaluates an arithmetic expression ($((...)) syntax, already
 * expanded); returns -1 after reporting an error. Used by `let`. */
int arith_evaluate(const char *expr, long *result);

/* ---- subst.c: $(...) and `...` ----
 *
 * Runs cmd in a forked subshell and returns its output (heap) without
 * trailing newlines. Sets $? to the subshell's status. */
char *command_substitute(const char *cmd);
/* <(cmd) (to_cmd 0) or >(cmd) (to_cmd 1): starts cmd on a pipe and
 * returns the shell's end, open until process_close_from(mark). */
int   process_substitute(const char *cmd, int to_cmd);
int   process_subst_mark(void);
void  process_close_from(int mark);

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

void alias_load_defaults(void);
void print_single_quoted(const char *s); /* 'value', a ' inside as '"'"' (vars.c) */
void alias_set(const char *name, const char *value);
void alias_unset(const char *name);
const char *alias_get(const char *name);
void alias_print(const char *name, const char *value);
void alias_print_all(void);

/* ---- history.c ----
 * One entry per command (multi-line commands included) in memory and in
 * $HISTFILE; $HISTSIZE, $HISTCONTROL and $HISTIGNORE as in bash. */
void history_load(void);                 /* at interactive startup; saves at exit */
void history_begin(const char *line);    /* the first line of a new command */
void history_continue(const char *line); /* a further line of the same command */
void history_finish(void);               /* the command is complete: write it out */
int  history_length(void);
const char *history_get(int i);          /* 0 = oldest */
int  history_number(void);             /* the number the next entry gets */
int  history_builtin(int argc, char **argv);
int  fc_builtin(int argc, char **argv);

/* ---- main.c ---- */

/* Runs a script file in this shell (scripts, ~/.acshrc, `source`).
 * Returns the last command's status, or -1 if it cannot be opened. */
int run_script_file(const char *path);

/* 1 in the interactive shell; 0 in scripts and subshells. */
int  shell_is_interactive(void);
void shell_mark_subshell(void);

/* Shell options (set -e, -u, -x, -f, -n, ... and -c, -i, -s at startup). */
int  shell_option(char c);
void shell_set_option(char c, int on);
const char *shell_option_string(void);  /* $- */

/* limits.c: a guard against running out of C stack on deeply nested
 * input. stack_init() at the top of main(); stack_exhausted() is true
 * when the recursion should stop with an error. */
void stack_init(void *base);
int  stack_exhausted(void);

/* prompt.c: PS1/PS2 with bash's backslash escapes (\u \h \w \$ \g for
 * the git branch, \? ...) and then expansion. */
char  *prompt_expand(const char *ps);   /* heap; may hold no-width markers */
void   prompt_print(const char *which); /* "PS1" or "PS2", to stderr */
char  *prompt_string(const char *which);/* expanded, heap (for the line editor) */
size_t prompt_width(const char *s);     /* columns the expanded prompt's last line takes */

/* lineedit.c: the interactive line editor (raw terminal mode).
 * lineedit_read returns the line with its newline, "" when Ctrl-C
 * cancelled it, NULL at end of input. */
#ifdef ACSH_MINIMAL  /* make MINIMAL=1: plain line reading only */
static inline int   lineedit_usable(void) { return 0; }
static inline char *lineedit_read(const char *prompt) { (void)prompt; return 0; }
#else
int   lineedit_usable(void);
char *lineedit_read(const char *prompt);
#endif

/* complete.c: Tab completion. Given the line and the cursor, returns the
 * new line (heap) and sets *new_pos, or NULL when nothing changes. */
char *lineedit_complete(const char *line, size_t pos, size_t *new_pos);
void alias_each(void (*fn)(const char *name, void *ctx), void *ctx);
void function_each(void (*fn)(const char *name, void *ctx), void *ctx);
void var_each(void (*fn)(const char *name, void *ctx), void *ctx);
void builtin_each(void (*fn)(const char *name, void *ctx), void *ctx);

#endif /* ACSH_H */
