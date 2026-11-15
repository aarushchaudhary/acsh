#!/bin/sh
# The community re-check: every complaint from the plan's phase 3, with
# the checks that show it fixed, run on a given binary (the release
# build). It copies the tests next to that binary, runs every suite,
# and prints one row per complaint: how many of its checks passed.
#
#   tests/complaints.sh BINARY [--minimal]
#
# A complaint whose checks did not run here (the root-only ones, say)
# is reported as "not run here" rather than as passed.

BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
MIN=$2
repo=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp -r "$repo/tests" "$repo/test_acsh.sh" "$repo/config" "$work/"
cp "$BIN" "$work/acsh"
cc -O2 -std=c11 -o "$work/pty_test" "$repo/tests/pty/pty_test.c" || exit 2

log=$work/all.log
(
    cd "$work"
    ./acsh test_acsh.sh 2>&1
    tests/run_diff.sh ./acsh 2>&1
    ./pty_test $MIN ./acsh 2>&1
    tests/bash_extras/run.sh ./acsh 2>&1
    tests/realworld/run.sh ./acsh 2>&1
) | grep -E '^(PASS|FAIL|SKIP): ' | grep -vE '^(PASS|FAIL): [0-9]+$' |   # not test_acsh's totals
    sed 's/^\(....\): */\1 /' > "$log"

bad=0
row() { # row STEP COMPLAINT PATTERN: the checks whose names match PATTERN
    p=$(grep -E "^PASS ($3)" "$log" | wc -l)
    f=$(grep -E "^FAIL ($3)" "$log" | wc -l)
    s=$(grep -E "^SKIP ($3)" "$log" | wc -l)
    if [ "$f" -gt 0 ]; then verdict="FAIL ($f failed)"; bad=1
    elif [ "$p" -eq 0 ]; then verdict="not run here"
    else verdict="fixed"
    fi
    extra=""
    [ "$s" -gt 0 ] && extra=", $s skipped"
    printf '| %-4s | %-58s | %3d passed%s | %s |\n' "$1" "$2" "$p" "$extra" "$verdict"
}

echo "| Step | Complaint | Checks | Verdict |"
echo "|---|---|---|---|"
row 3.1 "long-string segfaults; ash CVEs in IFS and recursion" \
    "100,000 nested|200,000 nested|a 100,000-long|a 100,001-term|a 1 MB value|arithmetic overflow|endless|a 50,000-long|[a-z0-9-]*\.sh runs to the end"
row 3.2 "~/.ashrc ignored without ENV; aliases lost under su, docker exec" \
    "a plain interactive shell|the default aliases are there|\\\$ENV is expanded|/etc/acshrc|aliases from /etc/acshrc|interactive shell runs|an interactive shell sets PS1"
row 3.3 "history: mc pollution, output on stderr, corrupted files" \
    "a line starting with a space|HISTIGNORE|repeated commands|a multi-line command|history prints|fc -|two shells writing|the file is trimmed|HISTFILE="
row 3.4 "an exported PS1 breaks other shells" \
    "the default prompt shows|outside a repository|PS1 escapes|PS2 is used|PS1 is never exported"
row 3.5 "bash scripts fail confusingly" \
    "a=\\(1 2\\)|\\\$\\{a\\[1\\]\\}|a\\[1\\]=x|select:|declare:|mapfile:|coproc:|\\\$\\{x\\^\\^\\}|\\\$\\{!x\\}"
row 3.6 "container scripts written for bash" \
    "braces matches bash|here_strings matches bash|printf_q matches bash|<<< |\\{a,b,c\\}|\\{1\\.\\.10|\\{a\\.\\.e\\}|quoted braces|assignments are not brace|set \\+B|printf %q"
row 3.7 "ash isn't a daily driver; no line editing" \
    "Left moves|Ctrl-|Home and End|Backspace|Alt-B|UTF-8: the cursor|a prompt wider|a line wider|Up recalls|Down comes|TERM=dumb|lineedit tests"
row 3.8 "completion covers only commands and files" \
    "cd sr<Tab>|\\\$PA<Tab>|\\\$\\{HOM<Tab>|a command name completes|a function name|an alias name|a name with a space|apk upg|when Tab adds nothing|complete tests"
row 3.9 "daily interactive use: history suggestions" \
    "suggestions are off|with set -o suggest|Right arrow accepts|the suggestion goes away|suggest tests"
row 3.10 "ash is valued for being small" \
    "a MINIMAL build reads plain lines"
echo
total_fail=$(grep -c '^FAIL' "$log")
echo "all checks on $(basename "$BIN"): $(grep -c '^PASS' "$log") passed, $total_fail failed, $(grep -c '^SKIP' "$log") skipped"
[ "$bad" -eq 0 ] && [ "$total_fail" -eq 0 ]
