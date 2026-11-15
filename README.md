# acsh

A small POSIX shell in C that aims to be a drop-in alternative to BusyBox
`ash`: the same language, the same builtins and the same output, plus a
handful of fixes for things `ash` users regularly run into.

Every claim below points at the test that checks it. Statements about
`ash` were checked against BusyBox `ash` 1.37, the reference the
differential tests run against locally; CI runs them against the busybox
that Ubuntu and Alpine ship (1.36).

**Status:** the scripting language matches `ash` (phase 2 of the
roadmap), and phase 3 made it a daily-driver interactive shell: a line
editor with history search, Tab completion, bash-style prompts, history
done right, startup files that work under `su` and `docker exec`, and a
hardened core (fuzzed, sanitizer-clean, no crash on hostile input). See
[What ash still does better](#what-ash-still-does-better).

---

## Building and running

```sh
make            # gcc or clang, any libc; no libraries needed
./acsh          # interactive shell
./acsh script.sh arg1 arg2
./acsh -c 'echo "$0 got $1"' name first
```

Invocation follows `ash`:

- `-c`, `-s`, `-i` and `-l` (login: reads `/etc/profile`, then `~/.profile`).
- Any `set` letter can be passed on the command line, for example `-e`, `-x` or `-u`.

Tested by `test_acsh.sh` sections [2.22] and [2.23].

The build is a single binary of about 195 KB (stripped, `-O2`, glibc);
`make MINIMAL=1` leaves out the line editor, completion and suggestions
(about 180 KB) and reads plain lines, as `ash` without editing does.
Nothing is installed and your login shell is not changed.

`make static` builds `acsh-static`, a self-contained binary with no
shared libraries, for containers and rescue systems. On Alpine it links
musl as is; on a glibc system install musl-tools and run
`make static CC=musl-gcc` (a glibc static build works, but warns that
`~user` lookups need glibc's NSS libraries at run time).

---

## How it is tested

| Suite | What it checks |
|---|---|
| `tests/run_diff.sh` | Runs each of the 47 scripts in `tests/diff/` under acsh **and** under BusyBox `ash`, and requires identical stdout and exit status. |
| `./acsh test_acsh.sh` | 117 checks, mostly of features that `ash` does not share (so they cannot be diffed), plus interactive behaviour through `-i`. |
| `tests/pty/pty_test` | A C program that runs acsh on a pseudo-terminal and types keys: startup files (including `/etc/acshrc` and `su`, as root in CI), prompts, the line editor, completion and suggestions. |
| Sanitizers and fuzzing | CI runs both suites under ASan/UBSan/LSan and fuzzes the parser and expansion with libFuzzer (`make fuzz`) for 2 minutes each on every push. |
| CI | `.github/workflows/ci.yml`, on every push: both suites on Ubuntu; both suites on Alpine 3.20 (musl) against Alpine's own busybox `ash`, plus a login shell running Alpine's `/etc/profile`; and the static musl binary run in an empty `FROM scratch` container. |

Run them yourself:

```sh
make check            # builds acsh and the pty tester, runs all three suites
```

Everything needed is a C compiler, `make` and busybox (for the
comparison with `ash`); there is no Python or other runtime.

---

## What acsh implements (same as ash)

Every item in this table is covered by a differential test, so it behaves as `ash` does.

| Area | Details | Tests (`tests/diff/`) |
|---|---|---|
| Grammar | pipelines, `!`, `&&` `\|\|`, `;` `&`, `( )`, `{ }`, `if`, `while`, `until`, `for`, `case` (including `(pat)`), functions (`f() {}` and `function f {}`), aliases (including values ending in a blank and aliases inside alias text) | `grammar_*`, `script_aliases`, `posix_misc`, `ash_extensions2` |
| Quoting | `'...'`, `"..."`, `\`, `$'...'` | `grammar_quoting`, `ash_extensions2` |
| Parameters | `$1`…, `$#`, `$@`, `$*`, `$?`, `$$`, `$!`, `$-`, `$0`, `$PPID`, `$LINENO`, `$RANDOM`, `$EPOCHSECONDS` and `$EPOCHREALTIME` (busybox 1.37 and later) | `script_args`, `ash_extensions*`, `builtins_env`, test_acsh [2.28] |
| Expansions | `${x:-}` `${x:=}` `${x:?}` `${x:+}` `${#x}` `${x#}` `${x##}` `${x%}` `${x%%}` `${x:off:len}` `${x/a/b}` `${x//a/b}`, tilde expansion, `$(...)` and backticks, `<(...)` and `>(...)` | `expand_*`, `ash_extensions*` |
| Arithmetic | full C operator set, assignments, `**`, `,`, `?:`, `16#ff` bases, variables evaluated as expressions | `expand_arith`, `ash_extensions2` |
| Field splitting and globbing | `$IFS` rules, `*` `?` `[...]`, `[[:class:]]`, `set -f`; `${#x}` counts characters in a UTF-8 locale | `expand_split`, `glob_many`, `posix_misc`, `ash_extensions2` |
| Redirections | `<` `>` `>>` `>\|` `<>` `<&` `>&` `&>`, here-documents `<<` `<<-` (quoted and unquoted) | `redirections_fd`, `heredocs`, `redirect_*` |
| Options | `set -aefnuvxC`, `-b`, `set -o pipefail`, `set -o`/`+o` listings (plus `-B`/`braceexpand`) | `set_options`, `set_errexit`, `set_noexec` |
| Traps and signals | every signal by name or number, `EXIT`, `ERR`, `''` to ignore (inherited by commands), `-` to reset, listing, traps reset in subshells, `wait` interrupted by a trapped signal; signals ignored on entry stay ignored, background jobs in scripts ignore `SIGINT`/`SIGQUIT` | `traps`, test_acsh [2.29] |
| Variables | `export` (`-n`, `-p`), `readonly`, `local`, `unset -f/-v`, temporary `VAR=x cmd` assignments, special builtins keeping them | `vars_scope`, `prefix_assign_pipe` |
| Builtins | `: . [ [[ alias bg break cd chdir command continue echo eval exec exit export false fg getopts hash help history jobs kill let local printf pwd read readonly return set shift source test times trap true type ulimit umask unalias unset wait` | `builtins_*`, `test_builtin`, `posix_misc` |
| Job control | process groups, `fg`, `bg`, `jobs -l/-p`, `%n %+ %- %name %?text`, `kill %job`, Ctrl-Z, the terminal handed over and taken back (termios saved per job) | `builtins_jobs`, test_acsh [2.21] |
| `cd` and `pwd` | logical paths through symlinks, `-P`/`-L`, `cd -`, `$CDPATH` | `builtins_env` |
| `read` | `-r -p -t -n -s -u -d`, IFS splitting, `$REPLY` | `builtins_io` |
| `umask` | octal, symbolic (`u=rwx,g=rx,o=`, `a-w`), `-S` | `builtins_env` |
| Startup | login shells read `/etc/profile` and `~/.profile`; interactive shells run `$ENV` | test_acsh [2.23], [2.28], `tests/pty` |

The details, mapped to the POSIX standard section by section, are in
[docs/posix_compliance.md](docs/posix_compliance.md).

---

## Where acsh goes further than ash

These fix specific things `ash` gets wrong or does not support. `ash`
fails each example below; acsh's test is named next to it.

| acsh | `ash` 1.37 | Test |
|---|---|---|
| `"${@:2:2}"` gives the 2nd and 3rd arguments | gives the 2nd and an empty word | test_acsh [2.25] |
| `${x/#pat/r}` and `${x/%pat/r}` anchor at the start and end | treats `#` and `%` as literal text | [2.25] |
| `[[ ( a || b ) && c ]]`, `[[ a < b ]]` | rejects `(` and parses `<` as a redirection | [2.24] |
| `case` clauses ending in `;&` fall through (added in POSIX.1-2024) | syntax error | [2.27] |
| `&>>file` appends stdout and stderr | syntax error | [2.27] |
| `$'...'` also supports `\u` and `\U` (UTF-8), `\e`, `\cX`, and `\0` ending the string, as in bash | prints `\u` and `\e` literally | [2.27] |
| errors name the file and line, and syntax errors show the correct form | bare `syntax error: unexpected ...` | [1.4], [2.26] |
| every interactive shell, login or not (`su`, `docker exec -it`, `lxc exec`), gets `ll`, `la`, `..` and reads `/etc/acshrc`, `~/.acshrc`, then `$ENV`; each file once | no aliases, and no rc file unless `ENV` is set | [1.1], [1.3], `tests/pty` |
| `trap -p SIGNAL` lists one trap (POSIX.1-2024) | `illegal option -p` | [2.27] |
| deep nesting (100,000 levels), 1 MB values and arithmetic overflow end in a clean error | `$((a=a=…=1))` with 100,000 `a=` segfaults | test_acsh [3.1] |
| a line editor: arrows, Home/End, Ctrl-A/E/B/F/U/K/W/Y, Alt-B/F, Ctrl-R search, Up/Down history, wide lines scroll; Tab completes commands, files, builtins, aliases, functions, `$VAR` names, directories after `cd` and `apk` subcommands; `set -o suggest` shows fish-style history suggestions (Right accepts) | busybox's line editor completes commands and files; no history suggestions | `tests/pty` (lineedit, complete, suggest) |
| `PS1` takes bash's escapes, `$(...)`, `\?` (last status) and `\g` (git branch, read from `.git/HEAD`, no `git` run); `PS1`/`PS2` are never exported | an exported `PS1` reaches every child shell, which may not understand it | `tests/pty` (prompt) |
| `<<<` here-strings, `{a,b}` / `{1..10..2}` / `{a..e}` brace expansion, `printf %q`, all matching bash | `syntax error: unexpected redirection`, `{a,b}` stays literal, `invalid format` | [3.6] |
| bash-only syntax gets a message naming the feature and the POSIX way: `a=(1 2)` → "arrays are a bash feature; use a space-separated list"; also `${a[i]}`, `select`, `declare`, `mapfile`, `coproc`, `${x^^}`, `${!x}` | `syntax error: unexpected "("`, `declare: not found` | [3.5] |
| history keeps a multi-line command as one entry, honours `HISTCONTROL`/`HISTIGNORE`, locks the file so parallel shells don't corrupt it, and has `fc` and `history -a -n -c -d -r -w` | a plain list; no `fc` | [3.3] |

Example of an acsh syntax error:

```
$ ./acsh -c 'if true
echo oops
fi'
acsh: -c: line 2: syntax error: expected 'then' after the 'if' condition
  acsh: usage:  if CONDITION; then COMMANDS; fi
```

---

## What ash still does better

These are known gaps:

- **vi editing mode.** acsh's line editor has emacs keys only;
  `set -o vi` is accepted but does nothing.
- **Speed.** BusyBox runs many applets (for example `ls` and `cat`)
  inside the shell process without forking; acsh always runs external
  programs. On a pure arithmetic loop (`while [ $i -lt 100000 ]`) acsh
  takes about 1.6× as long as `ash` (0.25 s against 0.16 s on the test
  machine).
- **Memory.** Peak RSS for a small script is about 1.8 MB, against 1.6 MB
  for `ash`.
- **`set -m`** cannot be toggled; job control is on exactly when the
  shell is interactive. `set -b` is accepted, but job changes are still
  reported at the next prompt.
- **Ctrl-C in scripts.** When a foreground command dies from `SIGINT`,
  a non-interactive acsh stops the script, as bash does; `ash` goes on
  to the next command. POSIX allows either.
- **File descriptors above 9** cannot be used in redirections (`exec 10>file`).
- **Bytes `\x01`, `\x02`, `\x05` and `\x06`** inside quoted text are used
  internally as markers and are dropped (for example `$'\x01'`).

Arrays, `[[ ]]` without word splitting, `select`, `coproc` and `shopt`
are bash-only and not in acsh (or `ash`); acsh says so when a script
uses them (test_acsh [3.5]). Three cheap bash extras are in: `<<<`
here-strings, `{a,b}`/`{1..5}` brace expansion (`set +B` turns it off)
and `printf %q`, each checked against bash's output (test_acsh [3.6]).

---

## Project layout

```
acsh/
├── Makefile
├── README.md
├── docs/posix_compliance.md   POSIX sections → status → tests
├── config/acshrc.default      template for ~/.acshrc
├── include/
│   ├── acsh.h                 module API shared by all files
│   └── syntax.h               syntax tree, lexer and parser API
├── src/
│   ├── main.c                 options, startup files, the input loop
│   ├── lexer.c, lex.h         tokens, quoting, here-documents, $'...'
│   ├── parse.c                recursive-descent parser → syntax tree
│   ├── exec.c                 runs the tree: pipelines, redirections, jobs, set -e/-x
│   ├── expand.c               all expansions, arithmetic, field splitting, globbing
│   ├── vars.c                 variable table (export, readonly), the child environment
│   ├── env.c                  positional parameters, locals, special parameters
│   ├── builtins.c             the builtins (table-driven)
│   ├── printf.c               echo and printf
│   ├── test_builtin.c         test, [ and [[ ]]
│   ├── functions.c            shell functions
│   ├── subst.c                $(...) and <(...) / >(...)
│   ├── signals.c              signal table, traps
│   ├── jobs.c                 job table, job specs, job status lines
│   ├── alias.c                aliases and the default set
│   ├── history.c              history list and file, history and fc builtins
│   ├── prompt.c               PS1/PS2 escapes and expansion, git branch
│   ├── lineedit.c             the line editor (raw terminal, keys, Ctrl-R, suggestions)
│   ├── complete.c             Tab completion
│   └── limits.c               the stack guard against deeply nested input
├── tests/
│   ├── run_diff.sh            acsh vs busybox ash differential runner
│   ├── diff/*.sh              the differential test scripts
│   ├── pty/pty_test.c         interactive tests on a pseudo-terminal (C, POSIX pty)
│   └── fuzz/                  libFuzzer targets (make fuzz)
├── test_acsh.sh               acsh's own suite (run with ./acsh)
└── .github/workflows/         CI (ci.yml) and tagged releases (release.yml)
```

---

## Development

```sh
make clean && make                          # -O2 -Wall -Wextra, must stay warning-free
make static                                 # acsh-static (use CC=musl-gcc on glibc systems)
make MINIMAL=1                              # without the line editor, completion, suggestions
./acsh test_acsh.sh                         # acsh's own suite
tests/run_diff.sh                           # needs busybox
make check                                  # all three suites (with MINIMAL=1 too)
make fuzz && ./fuzz_parse -max_total_time=60 tests/diff   # libFuzzer (clang)
gcc -std=c11 -Iinclude -g -fsanitize=address,undefined -o /tmp/acsh-asan src/*.c
tests/run_diff.sh /tmp/acsh-asan            # same tests under the sanitizers
```

A differential test that exposes a known gap can be marked with a
`# xfail` line; the runner then reports it without failing, and fails
if it starts passing so the marker gets removed.

---

Author: Aarush Chaudhary · License: GPLv3
