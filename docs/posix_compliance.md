# acsh and POSIX

This page goes through the POSIX shell standard (POSIX.1-2024, XCU
chapter 2 "Shell Command Language", plus the shell-related utilities)
section by section. For each part it gives acsh's status and the tests
that check it.

- **Yes**: implemented and tested.
- **Partial**: works, with the limits stated.
- **No**: missing.

Tests are named by file: `tests/diff/NAME.sh`, where acsh must match
BusyBox `ash` exactly, or by section of `test_acsh.sh`, written `[N.N]`.

## Quoting

| Item | Status | Tests |
|---|---|---|
| Backslash, single quotes, double quotes | Yes | `grammar_quoting`, [2.2] |
| `$'...'` dollar-single-quotes (new in 2024) | Yes, plus bash's `\u`, `\U`, `\e`, `\cX` | `ash_extensions2`, [2.27] |
| Line continuation (backslash-newline) | Yes | `grammar_quoting` |

## Token recognition, reserved words, aliases

| Item | Status | Tests |
|---|---|---|
| Operators, `IO_NUMBER`, comments | Yes | `grammar_*`, `redirections_fd` |
| Reserved words (`if then else elif fi do done case esac while until for in { } !`) | Yes | `grammar_compound` |
| Alias substitution: aliases in scripts, a value ending in a blank checks the next word, aliases inside alias text, no self-recursion | Yes | `script_aliases`, [2.19] |
| Extensions: `function NAME { }`, `[[ ]]` | Yes (not POSIX) | `ash_extensions2`, `test_builtin` |

## Parameters and variables

| Item | Status | Tests |
|---|---|---|
| Positional parameters, including `${10}` | Yes | `script_args`, `posix_misc` |
| Special parameters `@ * # ? - $ ! 0` | Yes | `script_args`, `expand_params` |
| `ENV` (interactive shells only) | Yes | [2.28] |
| `HOME`, `IFS` (reset at startup), `PATH` (ash's default when unset, [2.28]), `PWD`, `OLDPWD`, `PPID`, `LINENO`, `OPTIND`, `OPTARG`, `CDPATH` | Yes | `builtins_env`, `builtins_getopts`, `ash_extensions*` |
| `PS2`, `PS4` | Yes | `set_options` (PS4), `ash_extensions2` (defaults); the PS2 prompt was checked by hand over a pty |
| `PS1` (expanded each time; bash's `\u \h \w \$ …` escapes too) | Yes; never exported, so other shells never see acsh's prompt syntax | `tests/pty` (prompt) |
| `HISTFILE`, `HISTSIZE` (plus bash's `HISTCONTROL`, `HISTIGNORE`) | Yes | [3.3] |
| `LANG`, `LC_ALL`, `LC_CTYPE` | Partial: only `${#x}` counts UTF-8 characters; patterns and field splitting work on bytes, as in `ash` | `ash_extensions2` |

## Word expansions

| Item | Status | Tests |
|---|---|---|
| Tilde expansion, including `~user` and after `:` in assignments | Yes | `expand_misc` |
| Parameter expansion: all POSIX forms (`- = ? +`, with and without `:`, `#`, `##`, `%`, `%%`, `${#x}`) | Yes | `expand_params`, `expand_errors*` |
| Command substitution `$(...)` and backticks, including `case` inside `$(...)` | Yes | `expand_misc`, `grammar_quoting`, [2.4] |
| Arithmetic expansion: the full C operator set, assignments, `++`/`--`, `?:`, `,`, constants in bases 8, 10, 16 and `N#digits` | Yes | `expand_arith`, `ash_extensions2` |
| Field splitting, including empty `IFS`, IFS whitespace vs other characters, and `"$@"` | Yes | `expand_split`, `posix_misc` |
| Pathname expansion and `case` patterns, including `[!...]`, `[[:class:]]` and `set -f` | Yes | `posix_misc`, `glob_many`, `set_options`, [2.6] |
| Quote removal | Yes | `grammar_quoting` |
| Extensions: `${x:o:l}`, `${x/p/r}` (with `//`, `/#`, `/%`), `<(...)`, `>(...)`, `**`, brace expansion `{a,b}` `{1..5}` (off with `set +B`) | Yes (not POSIX) | `ash_extensions*`, [2.25], [3.6] |

## Redirection

| Item | Status | Tests |
|---|---|---|
| `<` `>` `>\|` `>>` `<>` `<&` `>&` and `-` to close | Yes | `redirections_fd`, `redirect_*` |
| Here-documents `<<` and `<<-`, with quoted and unquoted delimiters, several on one line | Yes | `heredocs` |
| `set -C` (noclobber) | Yes | `set_options` |
| File descriptors above 9 | **No**: rejected with an error | — |
| Extensions: `&>`, `&>>`, `<<<` here-strings | Yes (not POSIX) | `ash_extensions2`, [2.27], [3.6] |

## Exit status and errors

| Item | Status | Tests |
|---|---|---|
| 126 (found but not executable) and 127 (not found) | Yes | `posix_misc` |
| 128+N for a command killed by signal N | Yes | `builtins_jobs`, `traps` |
| Errors in special builtins and expansions end a non-interactive shell | Yes | `expand_errors*`, `vars_scope` |
| Syntax errors: status 2, and a non-interactive shell exits | Yes | `grammar_errors*`, [2.26] |

## Shell commands

| Item | Status | Tests |
|---|---|---|
| Simple commands: search order (special builtin, function, builtin, `PATH`), temporary assignments, assignments kept for special builtins | Yes | `posix_misc`, `vars_scope`, `prefix_assign_pipe` |
| Pipelines, `!`, `set -o pipefail` (new in 2024) | Yes | [2.1], `set_options` |
| Lists: `;`, `&`, `&&`, `\|\|` | Yes | `amp_separator`, [2.5] |
| `( )`, `{ }`, `for` (including `for x do`), `case` (including `(pat)` and `;&`), `if`, `while`, `until` | Yes | `grammar_compound`, `posix_misc`, `builtins_env`, [2.7]–[2.11], [2.27] |
| Function definitions, with redirections on the body | Yes | `grammar_functions`, `oneline_function` |
| `break N`, `continue N`, `return` (also at the top of a sourced file) | Yes | `loop_cap`, `return_*`, `source_return` |

## Job control

| Item | Status | Tests |
|---|---|---|
| Interactive job control: process groups, the terminal handed to the foreground job, Ctrl-Z, `fg`, `bg`, `jobs`, job IDs (`%n %+ %- %name %?text`) | Yes | `builtins_jobs`, [2.21], pty tests run by hand |
| `set -m` / `set -o monitor` | **Partial**: accepted, but job control is on exactly when the shell is interactive | — |
| `set -b` / `set -o notify` | **Partial**: accepted; job status is still reported at the next prompt | `builtins_env` |

## Signals and error handling

| Item | Status | Tests |
|---|---|---|
| `trap` with names or numbers, `EXIT`, `''` to ignore, `-` to reset, listing; `trap -p SIG` (new in 2024) | Yes | `traps`, [2.27] |
| Signals ignored on entry to a non-interactive shell cannot be trapped | Yes | [2.29] |
| Asynchronous commands ignore `SIGINT` and `SIGQUIT` when job control is off | Yes | [2.29] |
| A trapped signal interrupts `wait` (status 128+N) | Yes | `traps` |
| Ignored signals are inherited by commands, including `trap '' INT` | Yes | [2.29] |

Where POSIX leaves a choice: when a foreground command is killed by
`SIGINT` (Ctrl-C), a non-interactive acsh stops the script, as bash
does. `ash` continues with the next command.

## Shell execution environment

| Item | Status | Tests |
|---|---|---|
| Subshells get a copy of variables, functions, cwd, umask and open files; traps with a command reset to default; ignored traps kept | Yes | `traps`, `vars_scope` |
| Command substitution and pipeline stages run in subshells | Yes | [2.4], [2.20] |

## Special builtins

`break : continue . eval exec exit export readonly return set shift
times trap unset`: **Yes**, all present.

- `type` reports them as special builtins.
- Their assignment and error rules are tested in `posix_misc`,
  `vars_scope` and `source_return`.

`set` accepts the letters `-abefnuvxBC` and these `-o` names:
`allexport braceexpand errexit ignoreeof monitor noclobber noexec noglob
notify nounset pipefail suggest verbose vi xtrace`.

- `ignoreeof` works in the line editor (Ctrl-D does not exit). `vi`
  is accepted but does nothing: the editor has emacs keys only.
- `suggest` (acsh's own) shows history suggestions while typing.
- `set -h` is not supported.

## Other utilities that must be builtins

| Utility | Status | Tests |
|---|---|---|
| `alias`, `unalias` | Yes | [2.19] |
| `cd` (`-L`, `-P`, `-`, `CDPATH`), `pwd` (`-L`, `-P`) | Yes | `builtins_env` |
| `command` (`-p`, `-v`, `-V`) | Yes | `builtins_env`, [1.6] |
| `false`, `true` | Yes | `posix_misc` |
| `fg`, `bg`, `jobs` (`-l`, `-p`), `kill` (`-s`, `-l`, `%job`), `wait` | Yes | `builtins_jobs` |
| `getopts` | Yes | `builtins_getopts` |
| `hash` (`-r`) | Yes | `posix_misc` |
| `read` (`-r`, plus `-d` from 2024 and the extensions `-p -t -n -s -u`) | Yes | `builtins_io` |
| `type` | Yes | [1.6] |
| `ulimit`, `umask` (octal, symbolic, `-S`) | Yes | `posix_misc`, `builtins_env` |
| `fc` (`-l -n -r -s -e`, `FCEDIT`) | Yes | [3.3] |
| `newgrp` | Not a builtin; the system's `newgrp` runs | — |
