#!/bin/sh
# Differential tests: run every tests/diff/*.sh under acsh and under the
# reference shell (busybox ash), and compare stdout plus exit status.
#
#   tests/run_diff.sh [path/to/acsh]      (default ./acsh)
#   ASH="busybox ash" tests/run_diff.sh   (reference shell; this is the default)
#
# A test whose file contains a line starting with "# xfail" is a known gap:
# a difference is reported as XFAIL and does not fail the run. If such a
# test starts matching, it is reported as XPASS and DOES fail the run, so
# the marker gets removed and the fix stays covered from then on.
# stderr is not compared: the two shells word their error messages differently.
# A test may raise its time limit with a line "# timeout: SECONDS" (default 30),
# and pass arguments to the script with a line "# args: WORD...".

ACSH=${1:-./acsh}
ASH=${ASH:-busybox ash}
TIMEOUT=${TIMEOUT:-30}

here=$(cd "$(dirname "$0")" && pwd)
ACSH=$(cd "$(dirname "$ACSH")" && pwd)/$(basename "$ACSH")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

pass=0; fail=0; xfail=0; xpass=0

run_one() { # shell-command test-file output-file
    rm -rf "$work/cwd" && mkdir "$work/cwd"
    limit=$(sed -n 's/^# timeout: *\([0-9][0-9]*\).*/\1/p' "$2" | head -1)
    args=$(sed -n 's/^# args: *//p' "$2" | head -1)
    # SHELL_UNDER_TEST: for tests that start the same shell again
    (cd "$work/cwd" && SHELL_UNDER_TEST=$1 timeout "${limit:-$TIMEOUT}" $1 "$2" $args </dev/null >"$3" 2>/dev/null; echo "exit=$?" >>"$3")
}

for t in "$here"/diff/*.sh; do
    name=$(basename "$t" .sh)
    run_one "$ASH" "$t" "$work/ash.out"
    run_one "$ACSH" "$t" "$work/acsh.out"
    known_gap=no
    grep -q '^# xfail' "$t" && known_gap=yes

    if cmp -s "$work/ash.out" "$work/acsh.out"; then
        if [ $known_gap = yes ]; then
            echo "XPASS: $name (now matches ash: remove its '# xfail' line)"
            xpass=$((xpass + 1))
        else
            echo "PASS:  $name"
            pass=$((pass + 1))
        fi
    else
        if [ $known_gap = yes ]; then
            echo "XFAIL: $name ($(grep '^# xfail' "$t" | head -1 | sed 's/^# xfail:* *//'))"
            xfail=$((xfail + 1))
        else
            echo "FAIL:  $name"
            diff -u "$work/ash.out" "$work/acsh.out" | sed 's/^/    /' | head -40
            fail=$((fail + 1))
        fi
    fi
done

echo "diff summary: $pass pass, $fail fail, $xfail known gaps, $xpass unexpected passes"
[ $fail -eq 0 ] && [ $xpass -eq 0 ]
