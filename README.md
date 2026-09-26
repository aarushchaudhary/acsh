# acsh

A lightweight POSIX shell written in C, designed as a friendlier drop-in for minimal environments like Alpine Linux. acsh implements ash's POSIX-compliant core faithfully, adds the specific interactive conveniences that Alpine users commonly miss, and explicitly does not chase bash's non-POSIX feature set — keeping a small footprint while closing the real usability gap.

## Why acsh

Alpine Linux's default shell (`ash`) is minimal by design. The community consistently reports the same pain points:

- No default aliases (`ll`, `la`, etc.) out of the box
- Switching shells leaves you with an empty, unhelpful rc file
- No persistent command history
- "Bare bones... charming, until it's annoying"

acsh fixes exactly these gaps without pulling in bash's bloat.

## Features

**Core shell mechanics (matching ash)**
- Interactive REPL with `acsh:path$` prompt showing current directory
- Script execution (`./acsh script.sh`)
- Pipelines (`cmd1 | cmd2 | cmd3`)
- I/O redirection (`<`, `>`, `>>`)
- Background execution (`&`) with job control (`jobs`, `fg`, `bg`)
- `&&` / `||` / `;` chaining with correct exit-status propagation
- Quoting: single quotes (literal), double quotes (`$VAR` expands), backslash escaping
- Variable expansion (`$VAR`, `${VAR}`, `$?`, `~`)
- Glob expansion (`*`, `?`, `[abc]`) — no match stays literal (POSIX)
- Comments (`#`)
- Signal handling: Ctrl+C/Ctrl+Z routed to foreground job, SIGCHLD reaping (no zombies)

**Control flow**
- `if` / `then` / `elif` / `else` / `fi` — single-line and multi-line, nested
- `while COND; do BODY; done`
- `until COND; do BODY; done`
- `for VAR in word...; do BODY; done` *(in progress)*

**Beyond ash — the additions**
- Built-in default aliases: `ll`, `la`, `..`, `...`, `grep` (all overridable)
- `~/.acshrc` startup file — sourced automatically; `config/acshrc.default` ships as a ready-to-use template
- Persistent command history in `~/.acsh_history` (200 entries, dedup, survives restarts)
- `source` / `.` builtin to re-source files interactively
- `$?` special variable reflecting the last pipeline's exit status

**Builtins**
`cd`, `pwd`, `exit`, `export`, `unset`, `alias`, `unalias`, `history`, `jobs`, `fg`, `bg`, `source` / `.`

## Build

Requires gcc and a POSIX C11 environment (standard on any Linux system).

```sh
make
```

Produces the `acsh` binary in the project root. Nothing is installed.

```sh
make clean
```

## Usage

```sh
./acsh            # interactive mode
./acsh script.sh  # run a script
```

First run setup:

```sh
cp config/acshrc.default ~/.acshrc
```

This gives you sensible defaults (colored `ls`, the `l` alias, `EDITOR=vi`) and is itself a demonstration of what ash users have to set up manually after switching shells.

## Explicitly out of scope

These are deliberate scope decisions, not accidental gaps:

- Arrays / associative arrays
- Brace expansion (`{1..10}`, `{a,b,c}`)
- Tab completion
- Process substitution (`<(cmd)`, `>(cmd)`)
- `select` loops, coprocesses
- `declare`/`typeset` attribute flags
- Full readline-style line editing (Ctrl+R search, kill-ring)
- `~username` tilde expansion (only bare `~` and `~/path`)
- `break`/`continue` inside loops

## Project structure

```
acsh/
├── Makefile
├── config/
│   └── acshrc.default
├── include/
│   └── acsh.h
└── src/
    ├── main.c          # REPL, startup sequence, .acshrc loading
    ├── parser.c        # tokenizer, quoting, $VAR/glob expansion, chain splitting
    ├── executor.c      # fork/exec, pipelines, process groups, &&/||/; dispatch
    ├── builtins.c      # all builtin commands
    ├── jobs.c          # background/stopped job table
    ├── signals.c       # SIGCHLD/SIGINT/SIGTSTP handlers
    ├── env.c           # $VAR/$?/~ expansion
    ├── alias.c         # alias table and line expansion
    ├── history.c       # ring buffer + ~/.acsh_history persistence
    ├── glob.c          # pathname expansion via POSIX glob()
    └── control_flow.c  # if/while/until/for constructs
```

## License

See [LICENSE](LICENSE).
