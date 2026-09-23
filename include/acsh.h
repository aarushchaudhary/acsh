#ifndef ACSH_H
#define ACSH_H

#include <sys/types.h>  /* pid_t */

#define ACSH_MAX_LINE      1024   /* max chars in one input line        */
#define ACSH_MAX_ARGS      64     /* max args per single command        */
#define ACSH_MAX_CMDS      16     /* max commands chained in a pipeline */
#define ACSH_MAX_ALIASES   32     /* max number of aliases stored       */
#define ACSH_MAX_JOBS      32     /* max number of tracked background/stopped jobs */

typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} JobState;

/*
 * One tracked job = one pipeline that was launched in the background
 * (with &) or that got stopped (Ctrl+Z). We track it by process group
 * ID (pgid), not individual pid, since a pipeline can be many
 * processes but they all share one pgid.
 */
typedef struct {
    int      id;                  /* job number shown to the user, e.g. [1] */
    pid_t    pgid;                /* process group ID of the whole pipeline */
    pid_t    pids[ACSH_MAX_CMDS]; /* individual pids in the pipeline */
    int      num_pids;
    JobState state;
    char     cmdline[ACSH_MAX_LINE]; /* original text, for `jobs` output */
    int      in_use;              /* 0 = free slot in the table */
} Job;

/*
 * A single command in a pipeline, e.g. in `ls -l | grep foo`,
 * "ls -l" and "grep foo" are each represented by one Command.
 */
typedef struct {
    char *argv[ACSH_MAX_ARGS];   /* argv[0] = command name, NULL-terminated */
    int   argc;

    char *infile;                /* filename for  <   (NULL if none) */
    char *outfile;                /* filename for  >   (NULL if none) */
    int   append;                 /* 1 if redirection was >> instead of > */
} Command;

/*
 * A full parsed line: one or more Commands connected by pipes,
 * plus whether the whole pipeline should run in the background.
 */
typedef struct {
    Command cmds[ACSH_MAX_CMDS];
    int     num_cmds;
    int     background;           /* 1 if line ended with & */
    char    raw_line[ACSH_MAX_LINE]; /* original text, used for job listing */
} Pipeline;

/* Parsing entry point: fills `pl` from a raw input line. Returns 0 on
 * success, -1 on a parse error (message already printed). */
int parse_line(char *line, Pipeline *pl);

/* Execution entry point: runs a fully parsed pipeline. */
void execute_pipeline(Pipeline *pl);

/* Builtin dispatch: returns 1 and runs the builtin if cmd->argv[0] is
 * one of acsh's builtins, otherwise returns 0 and does nothing. */
int try_run_builtin(Command *cmd, int *exit_status);

/* ---- jobs.c: background/stopped job table ---- */

/* Registers a newly-launched pipeline as a job. Returns the job's id. */
int  jobs_add(pid_t pgid, pid_t pids[], int num_pids, const char *cmdline);

/* Marks a job (by pgid) as done and, if it was a background job,
 * prints the "[n]+ Done   cmdline" notice ash/bash print. */
void jobs_mark_done(pid_t pgid);

/* Marks a job (by pgid) as stopped, e.g. after Ctrl+Z (SIGTSTP). */
void jobs_mark_stopped(pid_t pgid);

/* Marks a job (by pgid) as running again, e.g. after `bg`. */
void jobs_mark_running(pid_t pgid);

/* Removes finished jobs from the table entirely (called after they've
 * been reported once by `jobs`/reaping, to stop them cluttering the list). */
void jobs_cleanup_done(void);

/* Looks up a job by its shell-visible id (as used in "%1"). Returns
 * NULL if not found. */
Job *jobs_find_by_id(int id);

/* Returns the most recently added job still in the table, or NULL. */
Job *jobs_find_most_recent(void);

/* Prints the `jobs` builtin's listing. */
void jobs_print_all(void);

/* ---- signals.c ---- */

/* Installs SIGCHLD/SIGINT/SIGTSTP handlers for the shell process itself. */
void signals_init(void);

/* Tells the signal handlers which pgid is currently in the foreground,
 * so Ctrl+C/Ctrl+Z get routed to it. Pass 0 when nothing is running
 * in the foreground (back at the prompt). */
void set_foreground_pgid(pid_t pgid);

/* ---- env.c: variable expansion ----
 *
 * acsh keeps things simple: `export NAME=value` sets a real process
 * environment variable (via setenv), so $NAME is visible both to acsh
 * itself and to any program it execs -- there is no separate
 * "shell-local but not exported" variable class, unlike full POSIX
 * shells which distinguish plain assignment from `export`. This is a
 * deliberate scope-reduction for the mini-project. */

/* Expands $VAR and ${VAR} references found inside `word` (which may
 * have come from inside double quotes or be fully unquoted -- single-
 * quoted text should never be passed through this, since $ is literal
 * there). Returns a newly malloc'd string the caller must free(). */
char *env_expand(const char *word);

/* ---- alias.c ----
 *
 * A direct fix for the most-cited community complaint about ash: it
 * ships with zero default aliases (no `ll`, etc.) and no non-empty rc
 * file. acsh ships sane defaults and lets the user override/add their
 * own via `alias name=value` in ~/.acshrc or interactively. */

typedef struct {
    char name[64];
    char value[256];
    int  in_use;
} Alias;

/* Loads acsh's built-in default aliases (ll, la, ..., etc). Called
 * once at startup, before ~/.acshrc is read, so the user's rc file
 * can override any of these. */
void alias_load_defaults(void);

/* Adds or updates an alias. Used by both the `alias` builtin and by
 * alias_load_defaults(). */
void alias_set(const char *name, const char *value);

/* Removes an alias. Used by the `unalias` builtin. */
void alias_unset(const char *name);

/* Looks up an alias by name. Returns NULL if none exists. */
const char *alias_get(const char *name);

/* Prints all currently defined aliases (the bare `alias` builtin). */
void alias_print_all(void);

/* If argv[0] of `line`'s first word matches a defined alias, replaces
 * that first word with the alias's expansion text (which may itself
 * be multiple words) and returns a newly malloc'd line for the caller
 * to parse instead. If no alias matches, returns NULL and the caller
 * should just use the original line. Only expands once (no recursive
 * alias-of-an-alias chaining), which matches simple shells and avoids
 * infinite loops from self-referential aliases. */
char *alias_expand_line(const char *line);

/* ---- history.c ----
 *
 * Simple ring-buffer command history plus a persistent history file,
 * another directly-cited gap vs bash ("bare bones... annoying"). No
 * readline/up-arrow recall yet -- just storage, the `history`
 * builtin, and load/save across sessions. */

#define ACSH_HISTORY_SIZE 200

/* Loads ~/.acsh_history into memory. Called once at startup. */
void history_load(void);

/* Appends one command line to history (in memory; call
 * history_save_append() to persist it to disk immediately). Blank
 * lines and lines identical to the immediately preceding entry are
 * skipped, matching common shell behaviour (HISTCONTROL=ignoredups). */
void history_add(const char *line);

/* Appends the most recently added entry to ~/.acsh_history on disk.
 * Called right after history_add() so history survives even if the
 * shell exits abnormally, rather than only saving at clean exit. */
void history_save_append(void);

/* Prints the `history` builtin's numbered listing. */
void history_print_all(void);

/* ---- main.c ----
 *
 * Exposed so the `source` / `.` builtin (builtins.c) can run an
 * arbitrary script file through the exact same run_line() path used
 * for both the interactive prompt and ~/.acshrc, rather than
 * duplicating that logic. Returns 0 on success, -1 if the file
 * couldn't be opened (message already printed). */
int run_script_file(const char *path);

#endif /* ACSH_H */
