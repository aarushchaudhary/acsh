# acsh

A POSIX-style command shell written in C, built to match BusyBox `ash`'s
core behavior while fixing a specific set of usability gaps that Alpine
Linux users have reported with `ash` in practice.

acsh is **not** a bash clone and does not try to be. It targets `ash`'s
feature set deliberately, and documents exactly where it goes further
and exactly where it stops.

---

## Building and running

No installation, no `chsh`, nothing touches your login shell. acsh is
just a binary you compile and run.

```bash
make
./acsh
```

Requires only `gcc` and `make` — no external libraries, no package
manager dependencies beyond a standard C toolchain and libc (tested
against glibc; written to be portable to musl, Alpine's libc, since
that's the actual target environment this project is framed around).

To get a sane set of default aliases and environment variables, copy
the shipped template once:

```bash
cp config/acshrc.default ~/.acshrc
```

---

## Project layout

```
acsh/
├── Makefile
├── README.md
├── config/
│   └── acshrc.default      shipped ~/.acshrc template
├── include/
│   └── acsh.h               shared structs and the full module API
└── src/
    ├── main.c                REPL loop, startup sequence, trap wiring
    ├── parser.c               tokenizer, quoting, expansion dispatch, chain splitting
    ├── executor.c             fork/exec, pipes, redirection, job control, assignments
    ├── builtins.c             cd, export, jobs, read, exec, type, command, local, set, trap, ...
    ├── test_builtin.c         test / [ / [[ ]]
    ├── control_flow.c         if/while/until/for/case, break/continue
    ├── functions.c            shell function definitions and calls
    ├── env.c                  $VAR expansion, $?, positional parameters, locals
    ├── glob.c                 *, ?, [abc] pathname expansion
    ├── subst.c                $(...) and `...` command substitution
    ├── alias.c                alias table and line expansion
    ├── history.c              persistent command history
    ├── jobs.c                 background/stopped job table
    └── signals.c              SIGCHLD/SIGINT/SIGTSTP handling, trap support
```

---

## What acsh implements

**Core shell mechanics**
- Pipelines (`|`), redirection (`<`, `>`, `>>`)
- Sequencing and conditional chaining (`;`, `&&`, `||`)
- Background execution (`&`) with real job control: process groups,
  `jobs`/`fg`/`bg`, zombie-free `SIGCHLD` reaping
- Quoting (`'...'`, `"..."`, `\`) with correct POSIX semantics —
  including that `$` expands inside double quotes but not single quotes
- Variable expansion (`$VAR`, `${VAR}`), tilde expansion, `$?`
- Command substitution, `$(...)` and `` `...` ``, run in a true forked
  subshell so side effects don't leak into the parent
- Pathname expansion / globbing (`*`, `?`, `[abc]`)
- Bare `NAME=value` assignments, including `x=$(cmd)` with correct
  POSIX exit-status rules

**Control flow**
- `if` / `elif` / `else` / `fi`, including arbitrary nesting
- `while`, `until`, `for ... in ...; do ... done`
- `case ... in ... esac` with glob-style patterns and `|` alternation
- `break [N]` / `continue [N]`, correctly scoped through nested loops
  and through `if`/`case` bodies

**Functions**
- `name() { ... }` definitions, same-line or multi-line
- `$1`...`$9`, `$#`, `$@` positional parameters
- Real recursion, with a depth guard that fails cleanly instead of
  crashing the shell

**Builtins**
```
cd        exit      pwd       export    unset     alias     unalias
jobs      fg        bg        history   source .  read      exec
umask     type      command   shift     local     set       eval
trap      break     continue  test  [   [[ ]]
```

---

## What `ash` has that acsh does not

These are real, acknowledged gaps — scoped out deliberately, not
overlooked:

| Missing | Notes |
|---|---|
| True arrays / associative arrays | Non-POSIX, bash-only in practice |
| Brace expansion (`{1..10}`, `{a,b,c}`) | Non-POSIX convenience |
| `getopts` | Not implemented |
| Full `trap` signal table | acsh supports only `INT` and `EXIT` |
| Full `set` option handling (`-e`, `-x`, ...) | Accepted but no-ops; only `set -- ARGS` has real effect |
| `[[ ]]` regex matching (`=~`) | Not implemented; `[[ ]]` supports the same operators as `test` plus `==` |
| Quoting inside a `for` word list | A quoted multi-word item incorrectly splits |
| Tab-completion, readline-style line editing | No up-arrow history recall, no `Ctrl+R` |
| `$RANDOM`, `$SECONDS`, `$BASHPID` | Not implemented |
| Process substitution, `select`, `coproc` | Not implemented |

---

## What acsh has that `ash` does not

This is acsh's actual reason to exist: a set of fixes aimed directly at
**specific, sourced complaints** from the Alpine/BusyBox community about
real friction points in `ash`, not a vague "more features" pitch.

### 1. Default aliases
`ash` ships with **zero** aliases out of the box — not even `ll`.
acsh loads a small, overridable set at startup:
```
ll    -> ls -l
la    -> ls -la
..    -> cd ..
...   -> cd ../..
grep  -> grep --color=auto
```

### 2. A real, non-empty startup file
On Alpine, switching to another shell commonly leaves you with an
**empty** rc file, and `ash` itself doesn't read a startup file at all
for non-login shells without manually setting `ENV` in `~/.profile`.
acsh auto-loads `~/.acshrc` unconditionally, with a real shipped
template (`config/acshrc.default`) that sets a few sane aliases and
`$EDITOR` — no `ENV` workaround required.

### 3. Persistent command history
`ash` has no history file and no `history` command. acsh keeps a
ring-buffer history in memory, persists it to `~/.acsh_history` as
you type, reloads it on the next session, and exposes it via the
`history` builtin.

### 4. Clear, actionable syntax errors
`ash` reports a bare `syntax error` with no further context. Every
acsh control-flow error names what was expected **and** shows the
correct syntax inline:
```
$ if true
  echo oops
  fi
acsh: syntax error: expected 'then' after the 'if' condition
  acsh: usage:  if CONDITION; then COMMANDS; fi
```

### 5. `[[ ]]` extended test
BusyBox `ash` does not include `[[ ]]` at all. acsh supports it as a
synonym for `test`/`[`, including `==` as an alias for `=`.

### 6. More informative `type`
acsh's `type` distinguishes and reports alias, shell function,
builtin, or `$PATH` location in one line; `ash`'s is comparatively
minimal.

---

## Known limitations (stated, not hidden)

- `$@` joins arguments with plain spaces rather than preserving
  per-word quoting
- `x=5 somecommand` (a command-scoped temporary assignment) is
  simplified to a normal persistent assignment
- `trap`'s `INT` handling is polled between prompts in the interactive
  loop; it is not re-checked inside a `source`d script mid-execution
- There is a small, acceptable simplification in how `test`/`[[ ]]`
  combine `-a`/`-o`: one level of combining is supported, not full
  POSIX precedence chains across many operands (real shells document
  this same ambiguity for deeply chained expressions)

---

## Development

```bash
make clean   # remove the built binary
make         # rebuild
```

No install step, no Docker required. For anyone targeting the
Alpine/musl environment specifically, building inside an Alpine
container or VM with `gcc`/`musl-dev`/`make` installed is enough to
validate musl-specific behavior; day-to-day development does not
require it.
