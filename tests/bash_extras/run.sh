#!/bin/sh
# The bash extras acsh takes on (here-strings, brace expansion, printf
# %q) against bash itself: each table here must give bash's output.
#   tests/bash_extras/run.sh [path/to/acsh]
ACSH=${1:-./acsh}
ACSH=$(cd "$(dirname "$ACSH")" && pwd)/$(basename "$ACSH")
here=$(cd "$(dirname "$0")" && pwd)
if ! command -v bash >/dev/null 2>&1; then
    echo "SKIP:  bash extras (bash is not installed)"
    echo "bash-extras summary: 0 pass, 0 fail, 1 skipped"
    exit 0
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
pass=0; fail=0
for t in "$here"/*.sh; do
    [ "$(basename "$t")" = run.sh ] && continue
    name=$(basename "$t" .sh)
    (cd "$work" && bash "$t" >"$work/bash.out" 2>&1; echo "exit=$?" >>"$work/bash.out")
    (cd "$work" && "$ACSH" "$t" >"$work/acsh.out" 2>&1; echo "exit=$?" >>"$work/acsh.out")
    if cmp -s "$work/bash.out" "$work/acsh.out"; then
        pass=$((pass + 1)); echo "PASS:  $name matches bash"
    else
        fail=$((fail + 1)); echo "FAIL:  $name matches bash"
        diff -u "$work/bash.out" "$work/acsh.out" | sed 's/^/    /' | head -20
    fi
done
echo "bash-extras summary: $pass pass, $fail fail, 0 skipped"
[ "$fail" -eq 0 ]
