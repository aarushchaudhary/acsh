#ifndef ACSH_H
#define ACSH_H

#include <sys/types.h>  /* pid_t */

#define ACSH_MAX_LINE      1024   /* max chars in one input line        */
#define ACSH_MAX_ARGS      64     /* max args per single command        */
#define ACSH_MAX_CMDS      16     /* max commands chained in a pipeline */
#define ACSH_MAX_ALIASES   32     /* max number of aliases stored       */
#define ACSH_MAX_JOBS      32     /* max number of tracked background/stopped jobs */
#define ACSH_MAX_LOOP_ITERATIONS 10000  /* safety cap against runaway while/until loops */

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

/*
 * How one Pipeline in a Chain is connected to the NEXT one. Mirrors
 * exactly how bash/ash decide whether to run the next stage:
 *   CHAIN_AND  ( && )  run next only if this one exited 0 (success)
 *   CHAIN_OR   ( || )  run next only if this one exited non-zero
 *   CHAIN_SEQ  ( ;  )  always run next, unconditionally
 *   CHAIN_END          this is the last pipeline in the chain
 */
typedef enum {
    CHAIN_AND,
    CHAIN_OR,
    CHAIN_SEQ,
    CHAIN_END
} ChainOp;

#define ACSH_MAX_CHAIN 16   /* max pipelines joined by &&/||/; on one line */

/*
 * A full input line, which may contain several pipe-groups joined by
 * &&, ||, or ;. E.g.  "make && ./acsh || echo failed"  becomes a
 * Chain of two raw segments: "make" --AND--> "./acsh || echo failed"
 * (further split lazily). Segments are stored as RAW TEXT, not
 * pre-parsed Pipelines: parsing (and therefore $VAR/$?/alias
 * expansion) must happen one stage at a time, immediately before that
 * stage runs, so that e.g. `false ; echo $?` sees $? from the `false`
 * that just ran in THIS SAME chain -- expanding every stage upfront
 * would use the previous LINE's $?, which is wrong.
 */
typedef struct {
    char    segments[ACSH_MAX_CHAIN][ACSH_MAX_LINE];
    ChainOp ops[ACSH_MAX_CHAIN];   /* ops[i] connects segments[i] to segments[i+1] */
    int     num_segments;
} Chain;

/* Parsing entry point: fills `pl` from a raw input line (a single
 * pipe-connected group only -- no &&/||/; splitting). Returns 0 on
 * success, -1 on a parse error (message already printed). */
int parse_line(char *line, Pipeline *pl);

/* Splits a raw input line on unquoted &&, ||, and ; into raw text
 * segments (NOT yet parsed into Pipelines -- see the Chain struct's
 * comment for why). Returns 0 on success, -1 on a syntax error (e.g.
 * an unterminated quote spanning the split points). */
int parse_chain(const char *line, Chain *ch);

/* Execution entry point for one pipe-connected group. Returns the
 * exit status of the pipeline's last command (0 = success, matching
 * POSIX $?), so execute_chain() can decide whether &&/|| should run
 * the next pipeline. For a backgrounded pipeline (trailing &), returns
 * 0 immediately without waiting, since there's nothing to report yet. */
int execute_pipeline(Pipeline *pl);

/* Runs every segment in `ch` in order: alias-expands, parses (which
 * performs $VAR/$?/tilde expansion via env_expand()), then executes
 * each segment ONE AT A TIME, honoring &&/||/; based on the PREVIOUS
 * segment's actual exit status from THIS SAME chain. This ordering
 * (parse-then-run per stage, not all stages upfront) is what makes
 * `false ; echo $?` correctly see $?==1. */
void execute_chain(Chain *ch);

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

/* Blocks/unblocks SIGCHLD so the async signal handler can't race with
 * a foreground pipeline's own explicit waitpid() loop in executor.c.
 * See the long comment above block_sigchld()'s definition for why
 * this is necessary. */
void block_sigchld(void);
void unblock_sigchld(void);

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
 * there). Also expands the special $? parameter to the exit status of
 * the last completed pipeline. Returns a newly malloc'd string the
 * caller must free(). */
char *env_expand(const char *word);

/* Records the exit status of the most recently completed pipeline, so
 * a later $? in env_expand() reports it. Called from execute_chain()
 * (executor.c) after each pipeline finishes. */
void env_set_last_status(int status);

/* Reads back the value env_set_last_status() most recently stored.
 * Used by control_flow.c to get a condition/body chain's exit status
 * without re-parsing "$?" as text. */
int env_get_last_status(void);

/* ---- glob.c: pathname expansion (*, ?, [abc]) ----
 *
 * A word containing an unquoted glob metacharacter (*, ?, [) is
 * expanded against the filesystem, e.g. `echo *.c` becomes `echo
 * main.c parser.c ...`. If the pattern matches nothing, POSIX leaves
 * the word UNCHANGED (not deleted, not an error) -- e.g. `echo *.xyz`
 * with no .xyz files just prints the literal string "*.xyz". */

/* Expands `word` (already $VAR/tilde-expanded, still possibly
 * carrying the QUOTED_FIRST marker from parser.c) into zero or more
 * matched paths. Writes up to max_results newly malloc'd strings into
 * `results` and returns how many it wrote. If `word` has no unquoted
 * glob metacharacters, or matches nothing, writes exactly one result:
 * `word` itself (duplicated), matching POSIX's "no match = literal"
 * rule. The caller owns and must free() every string written. */
int glob_expand(const char *word, char *results[], int max_results);

/* ---- control_flow.c: if / then / else / fi ----
 *
 * A deliberately scoped subset of shell control flow: only the
 * if/then/[elif/then...]/[else]/fi form is supported. while, until,
 * for, case, and function definitions are explicitly OUT OF SCOPE for
 * this mini-project (documented in docs/posix_compliance.md) -- they
 * require the same kind of "read until matching keyword" handling as
 * if/fi but each has enough of their own special cases (loop bodies
 * re-executing, case pattern matching, etc.) that adding all of them
 * was judged not to fit the project's time budget. if/fi was chosen
 * as the one control-flow construct to implement because it's the
 * most frequently used in real interactive/script usage.
 *
 * Supported forms (each clause's CONDITION and BODY are themselves
 * ordinary &&/||/;  chains, reusing everything in parser.c/executor.c):
 *
 *   if COND; then BODY; fi
 *   if COND; then BODY; else BODY; fi
 *   if COND; then BODY; elif COND; then BODY; ...; else BODY; fi
 *
 * Both single-line (all on one line, with explicit ; before then/
 * else/fi) and multi-line (real shell script style, newline-
 * separated) forms are supported, since run_if_block() reads
 * additional lines from stdin itself when the construct isn't closed
 * on the first line it sees. */

/* Returns 1 if `line` (with leading whitespace already skipped) looks
 * like the start of an if statement (i.e. its first word is "if"). */
int is_if_statement(const char *line);

/* Same idea as is_if_statement(), for while/until loops. */
int is_while_statement(const char *line);
int is_until_statement(const char *line);

/* Runs a full if/then/elif/else/fi construct. `first_line` is the
 * line that already matched is_if_statement(). If the construct isn't
 * fully closed within first_line (the common, multi-line script
 * case), additional lines are read one at a time via `read_more_line`
 * (typically fgets from stdin) until the matching `fi` is found.
 * Returns the exit status of whichever branch's body actually ran (0
 * if no branch matched and there was no else). */
int run_if_statement(const char *first_line,
                     char *(*read_more_line)(char *buf, int size));

/* Runs a full while/until...do...done loop construct. `first_line` is
 * the line that already matched is_while_statement() or
 * is_until_statement() (`is_until` says which). Reads additional
 * lines the same way run_if_statement() does when the loop isn't
 * closed on one line. The condition is re-evaluated, and the body
 * re-run, each iteration -- exactly like a real shell loop, including
 * picking up side effects (e.g. a counter variable incremented in the
 * body) between iterations. Returns the exit status of the last body
 * execution (0 if the body never ran, matching POSIX for a loop whose
 * condition was false/true-respectively from the start). A loop is
 * capped at ACSH_MAX_LOOP_ITERATIONS iterations as a safety net
 * against an unintentional infinite loop hanging the whole shell. */
int run_loop_statement(const char *first_line, int is_until,
                       char *(*read_more_line)(char *buf, int size));

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
