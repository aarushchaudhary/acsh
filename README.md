# acsh

A small POSIX shell in C that aims to be a drop-in alternative to BusyBox
`ash`: the same language, the same builtins and the same output, plus a
handful of fixes for things `ash` users regularly run into.

Every claim below points at the test that checks it. Statements about
`ash` were checked against BusyBox `ash` 1.37, the reference the
differential tests run against locally; CI runs them against the busybox
that Ubuntu and Alpine ship (1.36).

**Status:** the scripting language matches `ash` (phase 2 of the
roadmap). Interactive line editing, tab completion and `PS1` prompts are
the next phase and are **not** in acsh yet. See
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

The build is a single binary of about 160 KB (stripped, `-O2`, glibc).
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
| `./acsh test_acsh.sh` | 77 checks, mostly of features that `ash` does not share (so they cannot be diffed), plus interactive behaviour through `-i`. |
| Sanitizers | Both suites are also run against an ASan/UBSan/LSan build, with no reports. |
| CI | `.github/workflows/ci.yml`, on every push: both suites on Ubuntu; both suites on Alpine 3.20 (musl) against Alpine's own busybox `ash`, plus a login shell running Alpine's `/etc/profile`; and the static musl binary run in an empty `FROM scratch` container. |

Run them yourself:

```sh
make && ./acsh test_acsh.sh && tests/run_diff.sh
```

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
| Options | `set -aefnuvxC`, `-b`, `set -o pipefail`, `set -o`/`+o` listings | `set_options`, `set_errexit`, `set_noexec` |
| Traps and signals | every signal by name or number, `EXIT`, `ERR`, `''` to ignore (inherited by commands), `-` to reset, listing, traps reset in subshells, `wait` interrupted by a trapped signal; signals ignored on entry stay ignored, background jobs in scripts ignore `SIGINT`/`SIGQUIT` | `traps`, test_acsh [2.29] |
| Variables | `export` (`-n`, `-p`), `readonly`, `local`, `unset -f/-v`, temporary `VAR=x cmd` assignments, special builtins keeping them | `vars_scope`, `prefix_assign_pipe` |
| Builtins | `: . [ [[ alias bg break cd chdir command continue echo eval exec exit export false fg getopts hash help history jobs kill let local printf pwd read readonly return set shift source test times trap true type ulimit umask unalias unset wait` | `builtins_*`, `test_builtin`, `posix_misc` |
| Job control | process groups, `fg`, `bg`, `jobs -l/-p`, `%n %+ %- %name %?text`, `kill %job`, Ctrl-Z, the terminal handed over and taken back (termios saved per job) | `builtins_jobs`, test_acsh [2.21] |
| `cd` and `pwd` | logical paths through symlinks, `-P`/`-L`, `cd -`, `$CDPATH` | `builtins_env` |
| `read` | `-r -p -t -n -s -u -d`, IFS splitting, `$REPLY` | `builtins_io` |
| `umask` | octal, symbolic (`u=rwx,g=rx,o=`, `a-w`), `-S` | `builtins_env` |
| Startup | login shells read `/etc/profile` and `~/.profile`; interactive shells run `$ENV` | test_acsh [2.23], [2.28] |

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
| interactive shells get `ll`, `la`, `..` aliases and read `~/.acshrc`, with no `ENV` setup needed | no aliases, and no rc file unless `ENV` is set | [1.1], [1.3] |
| `trap -p SIGNAL` lists one trap (POSIX.1-2024) | `illegal option -p` | [2.27] |

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

- **Line editing.** `ash` has cursor movement, Up-arrow history recall,
  Ctrl-R search, tab completion and vi/emacs modes. acsh reads plain
  lines. It saves history to `~/.acsh_history` (test_acsh [1.2]) but
  cannot recall it with the arrow keys yet. This is the next phase.
- **`PS1`.** acsh prints its own fixed `acsh:DIR$ ` prompt and ignores
  `PS1`. `PS2` and `PS4` are honoured.
- **Speed.** BusyBox runs many applets (for example `ls` and `cat`)
  inside the shell process without forking; acsh always runs external
  programs. On a pure arithmetic loop (`while [ $i -lt 100000 ]`) acsh
  takes about 1.5× as long as `ash` (0.23 s against 0.15 s on the test
  machine).
- **Memory.** Peak RSS for a small script is about 1.9 MB, against 1.6 MB
  for `ash`.
- **`set -m`** cannot be toggled; job control is on exactly when the
  shell is interactive. `set -o vi`, `ignoreeof` and `set -b` are
  accepted but have no effect yet.
- **`fc`** and `$HISTFILE`/`$HISTSIZE` are not supported.
- **Ctrl-C in scripts.** When a foreground command dies from `SIGINT`,
  a non-interactive acsh stops the script, as bash does; `ash` goes on
  to the next command. POSIX allows either.
- **File descriptors above 9** cannot be used in redirections (`exec 10>file`).
- **Bytes `\x01`, `\x02`, `\x05` and `\x06`** inside quoted text are used
  internally as markers and are dropped (for example `$'\x01'`).

Bash-only features (arrays, `{a,b}` brace expansion, `[[ ]]` without
word splitting, `select`, `coproc`, `shopt`) are not in `ash` and not in
acsh.

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
│   └── history.c              ~/.acsh_history
├── tests/
│   ├── run_diff.sh            acsh vs busybox ash differential runner
│   └── diff/*.sh              the differential test scripts
├── test_acsh.sh               acsh's own suite (run with ./acsh)
└── .github/workflows/         CI (ci.yml) and tagged releases (release.yml)
```

---

## Development

```sh
make clean && make                          # -O2 -Wall -Wextra, must stay warning-free
make static                                 # acsh-static (use CC=musl-gcc on glibc systems)
./acsh test_acsh.sh                         # acsh's own suite
tests/run_diff.sh                           # needs busybox
gcc -std=c11 -Iinclude -g -fsanitize=address,undefined -o /tmp/acsh-asan src/*.c
tests/run_diff.sh /tmp/acsh-asan            # same tests under the sanitizers
```

A differential test that exposes a known gap can be marked with a
`# xfail` line; the runner then reports it without failing, and fails
if it starts passing so the marker gets removed.

---

Author: Aarush Chaudhary · License: GPLv3
