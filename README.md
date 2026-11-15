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
hardened core (24 CPU-hours of fuzzing and every hostile input tested,
with no crash: see [Evidence](#evidence)). See
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

`make` gives a single binary of about 195 KB (stripped, `-O2`, glibc).
The release build, `make static`, is built for size: about 130 KB of
code (121 KB with `MINIMAL=1`, which leaves out the line editor,
completion and suggestions and reads plain lines, as `ash` without
editing does). Measured sizes are under [Evidence](#evidence).
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
| `tests/run_diff.sh` | Runs each of the 50 scripts in `tests/diff/` under acsh **and** under BusyBox `ash`, and requires identical stdout and exit status. |
| `./acsh test_acsh.sh` | 130 checks, mostly of features that `ash` does not share (so they cannot be diffed), plus interactive behaviour through `-i`. |
| `tests/pty/pty_test` | A C program that runs acsh on a pseudo-terminal and types keys: startup files (including `/etc/acshrc` and `su`, as root in CI), prompts, the line editor, completion and suggestions (46 checks). |
| `tests/realworld/run.sh` | Real scripts under acsh and `ash`, compared: the system's `/etc/profile`, every init script in `/etc/init.d` (OpenRC's sourced, with `depend()` run), abuild and its `functions.sh`, and `./configure` of an autotools project generated on the spot. |
| `tests/bash_extras/run.sh` | The bash extras (`<<<`, brace expansion, `printf %q`) against bash itself, table by table. |
| `tests/crash/` | Known `ash` crash inputs (CVE-2022-48174's `${0::0/0~09J}`, and inputs aimed at the IFS-splitting and `evalcommand` heap overflows of busybox 1.38.0): each must run to the end. |
| `tests/complaints.sh` | The community re-check: every complaint the roadmap set out to fix, with how many checks show it fixed, run on the release build (`make recheck`). |
| `tests/bench/bench.sh` | Size, idle memory and 1,000 runs of `-c true` against a standalone static `ash` built from busybox source (CI, on Alpine), with the roadmap's size and speed gate. |
| Sanitizers and fuzzing | CI runs the suites under ASan/UBSan/LSan and fuzzes the parser and expansion with libFuzzer (`make fuzz`) for 2 minutes each on every push. |
| CI | `.github/workflows/ci.yml`, on every push: everything above on Ubuntu, on Alpine 3.20 (musl, against Alpine's own busybox `ash`, as root), on a `MINIMAL=1` build and under the sanitizers; the static musl binary run in an empty `FROM scratch` container. |

Run them yourself:

```sh
make check            # builds acsh and the pty tester, runs the suites
make recheck          # the community re-check on the release build
```

Everything needed is a C compiler, `make` and busybox (for the
comparison with `ash`); there is no Python or other runtime.

---

## Evidence

Measured on 2026-11-16 on an Intel Core Ultra 7 155H (22 threads,
Linux, glibc), against Ubuntu's BusyBox 1.37.0 `ash`. CI repeats the
size and speed comparison on Alpine against a standalone static `ash`
built from BusyBox source (`bench` job).

### Fuzzing: 24 CPU-hours, no crash

Both libFuzzer targets (`make fuzz`, built with AddressSanitizer and
UndefinedBehaviorSanitizer) ran for 72 minutes each with 10 parallel
workers, 20 workers in all: 24 CPU-hours. They started from the test
scripts and the known `ash` crash inputs in `tests/crash/`, and kept
going after any finding (`-ignore_crashes=1` and the like) so that every
problem would be recorded.

| Target | What it exercises | Inputs run | Code edges reached | Crashes | Hangs | Out of memory |
|---|---|---|---|---|---|---|
| `fuzz_parse` | lexer and parser, whole scripts | 21.9 million | 1,879 | 0 | 0 | 0 |
| `fuzz_expand` | every expansion and arithmetic | 147.5 million | 2,009 | 0 | 0 | 0 |

Coverage stopped growing about 25 minutes before the end. To run the
same campaign (a seed corpus can be any directory of shell scripts):

```sh
make fuzz
mkdir -p corpus && cp tests/diff/*.sh tests/crash/*.sh corpus/
./fuzz_parse  -fork=10 -max_total_time=4320 -timeout=10 -close_fd_mask=3 corpus/
./fuzz_expand -fork=10 -max_total_time=4320 -timeout=10 -close_fd_mask=3 corpus/
```

Before this campaign, earlier shorter runs and the deep-input tests of
phase 3 found and fixed: stack overflows on deeply nested input, a
`SIGFPE` on `$(( LONG_MIN / -1 ))`, signed-overflow undefined behaviour
in arithmetic, an exponent loop that never ended, quadratic slowdowns on
long values and long lists, and a memory leak in the parser. Each has a
test in `test_acsh.sh` [3.1] or `tests/crash/`. CI fuzzes both targets
for 2 more minutes on every push.

### Size, memory, startup against a standalone ash

The fair comparison: both statically linked with musl, BusyBox built
with only `ash` (with line editing and history, as Alpine configures
it), acsh with `make static` (`-Oz`, not position-independent, as
BusyBox's static build). `tests/bench/bench.sh`, best of three runs;
memory is the shell's own idle RSS, read with builtins only. Measured
locally with Ubuntu's musl-gcc 15; the CI `bench` job measures the same
on Alpine (its absolute numbers are a little smaller for both shells).

| Build | Size (stripped) | Idle RSS | 1,000 × `-c true` |
|---|---|---|---|
| BusyBox 1.36.1 `ash` only | 223,032 B | 264–268 KB | 244–276 ms |
| acsh `MINIMAL=1` | 243,408 B | 280 KB | 253–288 ms |
| acsh full | 251,600 B | 292–296 KB | 251–285 ms |

| Roadmap target | Result |
|---|---|
| minimal build no slower than `ash` | **met** (within 5%; with a full environment of 106 variables acsh starts faster: 1.22–1.29 s against 1.28–1.33 s for 5,000 runs) |
| full build within 1.5× of `ash`'s size | **met** (1.13×) |
| minimal build no bigger than `ash` | **not met**: 9% bigger (20 KB) |
| minimal build no heavier than `ash` | **not met**: 12–16 KB more RSS |

What closed most of the gap: building non-PIE and with `-Oz`; importing
the environment with 2 allocations instead of 3 per variable (startup
was 13–25% slower before); allocating the function-call frames and
seeding `$RANDOM` only when first needed (memory was 40 KB higher).
The rest is acsh's own code (about 20 KB more than `ash`'s); removing
features to close it was ruled out. The `bench` job reports the two
unmet targets as known gaps and fails if they are ever met, so the
gate can be made strict.

### The community complaints, re-checked

`tests/complaints.sh` (`make recheck`) runs every suite on the release
build and counts the checks behind each complaint the roadmap set out to
fix. On the full release build, 260 checks passed and none failed; the
5 skipped need root and run in the Alpine CI job.

| Step | Complaint | Checks passed |
|---|---|---|
| 3.1 | long-string segfaults; `ash` CVEs in IFS splitting and recursion | 14 |
| 3.2 | `~/.ashrc` ignored without `ENV`; aliases lost under `su`, `docker exec` | 5 (+2 as root in CI) |
| 3.3 | history: Midnight Commander pollution, output on stderr, corrupted files | 10 |
| 3.4 | an exported `PS1` breaks other shells | 5 |
| 3.5 | bash scripts fail confusingly | 9 |
| 3.6 | container scripts written for bash | 13 |
| 3.7 | `ash` isn't a daily driver; no line editing | 23 |
| 3.8 | completion covers only commands and files | 9 |
| 3.9 | history suggestions | 4 |
| 3.10 | `ash` is valued for being small | 1 (on the `MINIMAL=1` build, which passes 225 checks) |

### Found by running real scripts

Running Alpine's OpenRC scripts, abuild and autoconf `configure` scripts
under acsh (`tests/realworld/`, and libltdl's full `configure`: 121
checks, the same `config.h` as under `ash`) found five bugs that the
unit tests had missed. Each now has a test in `tests/diff/`
(`command_v`, `openrc_style`, `status_exit_exec`, `traps`):

| Found | Before | Now |
|---|---|---|
| `command -v f` for a function (how init scripts test for `depend`) | not found | prints `f`, as `ash` |
| `exec prog` after `export X=1` | `prog` got the environment acsh started with; `configure` re-ran itself forever | `prog` gets the shell's exported variables |
| `false; rc=$?` | `rc` was 0 | 1 |
| `exit 1` with `trap '... exit' 0` | the trap saw `$?=0`; the shell exited 0 | `$?` is 1 in the trap; a bare `exit` there keeps 1 |
| a trapped signal arriving as a background job ends, during `wait` | `wait` sometimes returned 0 | always 128+N, as `ash` |

### The final code audit

A whole-repo audit (correctness first, then safety, scale, tests, speed
and size) found 10 issues; all are fixed, each with a test in
`test_acsh.sh` [4.7] or `tests/bash_extras/`. A second pass found one
more and a third found nothing. The ones that changed behaviour:

| Found | Before | Now |
|---|---|---|
| `alias r='fc -s'`, `FCEDIT=... fc` | `fc` re-ran its own line until the depth limit | runs the previous command once |
| 65 functions, 33 aliases, a 256-character alias | "table full"; long values cut short silently | no limit |
| 33 background jobs, then `wait $!` | 127 ("unknown") | the job's status |
| `{1,2,...,300}` | 257 words, the last glued together | 300 words, as bash |
| a 300-character variable name | `$name` read the wrong variable; arithmetic failed | works, as in `ash` |

The lean findings removed about 27 lines (three copies of the quoting
code merged into one, a duplicate name check, dead code). Later passes
of the sanitizers and the fuzzers over the fixed code were clean.

---

## What acsh implements (same as ash)

Every item is covered by the tests named beside it: the `tests/diff/` scripts compare acsh's output
with `ash`'s, and the test_acsh sections check what a diff cannot (startup files, job control).

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
- **Size and memory of the smallest build.** Statically linked with musl,
  `make static MINIMAL=1` is 9% bigger than an `ash`-only BusyBox and
  uses 12–16 KB more memory; it starts as fast. See
  [Evidence](#evidence).
- **Speed of long scripts.** BusyBox runs many applets (for example
  `ls` and `cat`) inside the shell process without forking; acsh always
  runs external programs. On a pure arithmetic loop
  (`while [ $i -lt 100000 ]`) acsh takes about 1.6× as long as `ash`
  (0.13 s against 0.08 s). Starting up is not slower: see
  [Evidence](#evidence).
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
make check                                  # all suites (with MINIMAL=1 too)
make fuzz && ./fuzz_parse -max_total_time=60 tests/diff   # libFuzzer (clang)
gcc -std=c11 -Iinclude -g -fsanitize=address,undefined -o /tmp/acsh-asan src/*.c
tests/run_diff.sh /tmp/acsh-asan            # same tests under the sanitizers
```

A differential test that exposes a known gap can be marked with a
`# xfail` line; the runner then reports it without failing, and fails
if it starts passing so the marker gets removed.

---

Author: Aarush Chaudhary · License: GPLv3
