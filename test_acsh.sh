#!./acsh
#
# test_acsh.sh -- exercises every "beyond ash" feature claimed in
# README.md, plus the core features those improvements depend on.
#
# This file is itself an acsh script and only uses syntax acsh
# actually supports (see README.md "Known limitations": no arithmetic
# expansion, no quoting inside a `for` word list, etc).
#
# HOW TO RUN (from the project root, after `make`):
#
#       ./test_acsh.sh
#
# The shebang above is deliberately the RELATIVE path ./acsh rather
# than `/usr/bin/env acsh`: this project is never installed
# system-wide, so `acsh` is not on $PATH and `env` would fail with
# "No such file or directory". A relative interpreter path is
# resolved against the current directory, so this script must be
# launched from the project root -- which it needs anyway, because
# several checks below invoke ./acsh directly as a child process.
# `./acsh test_acsh.sh` works identically.
#
# Each check prints PASS or FAIL with a label. Results are also
# tallied into temp files so the summary at the end is an exact
# count rather than something to total by eye.

rm -f /tmp/acsh_test_pass_count /tmp/acsh_test_fail_count
touch /tmp/acsh_test_pass_count
touch /tmp/acsh_test_fail_count

# check_eq LABEL EXPECTED ACTUAL
check_eq() {
    if test "$2" = "$3"
    then
        echo "PASS: $1"
        echo x >> /tmp/acsh_test_pass_count
    else
        echo "FAIL: $1 (expected [$2], got [$3])"
        echo x >> /tmp/acsh_test_fail_count
    fi
}

echo "=================================================="
echo " acsh test suite"
echo "=================================================="

echo ""
echo "--- Section 1: beyond-ash features (README claims) ---"

echo ""
echo "[1.1] Default aliases (ll, la, .., ..., grep)"
alias | grep "^alias ll=" > /tmp/acsh_test_alias_check 2>/dev/null
if test -s /tmp/acsh_test_alias_check
then
    check_eq "default alias ll is defined at startup" "yes" "yes"
else
    check_eq "default alias ll is defined at startup" "yes" "no"
fi
alias | grep "^alias la=" > /tmp/acsh_test_alias_check 2>/dev/null
if test -s /tmp/acsh_test_alias_check
then
    check_eq "default alias la is defined at startup" "yes" "yes"
else
    check_eq "default alias la is defined at startup" "yes" "no"
fi
rm -f /tmp/acsh_test_alias_check

echo ""
echo "[1.2] Persistent history (~/.acsh_history gets written to)"
export HOME=/tmp/acsh_test_home
mkdir -p $HOME
echo "echo history-probe-line" > /tmp/acsh_test_hist_input
echo "exit" >> /tmp/acsh_test_hist_input
./acsh < /tmp/acsh_test_hist_input > /dev/null 2>&1
if test -f $HOME/.acsh_history
then
    grep -q "history-probe-line" $HOME/.acsh_history
    if test $? -eq 0
    then
        check_eq "command persisted to ~/.acsh_history" "yes" "yes"
    else
        check_eq "command persisted to ~/.acsh_history" "yes" "no (file exists, line missing)"
    fi
else
    check_eq "command persisted to ~/.acsh_history" "yes" "no (file never created)"
fi
rm -rf $HOME /tmp/acsh_test_hist_input

echo ""
echo "[1.3] Non-empty startup file support (~/.acshrc is read)"
export HOME=/tmp/acsh_test_home2
mkdir -p $HOME
echo "export ACSH_RC_PROBE=loaded" > $HOME/.acshrc
echo "echo rc-value: \$ACSH_RC_PROBE" > /tmp/acsh_test_rc_input
echo "exit" >> /tmp/acsh_test_rc_input
./acsh < /tmp/acsh_test_rc_input > /tmp/acsh_test_rc_output 2>&1
grep -q "rc-value: loaded" /tmp/acsh_test_rc_output
if test $? -eq 0
then
    check_eq "~/.acshrc loaded and export took effect" "yes" "yes"
else
    check_eq "~/.acshrc loaded and export took effect" "yes" "no"
fi
rm -rf $HOME /tmp/acsh_test_rc_input /tmp/acsh_test_rc_output

echo ""
echo "[1.4] Clear syntax errors (usage hint present, not a bare message)"
echo "if true" > /tmp/acsh_test_syntax_input
echo "echo oops" >> /tmp/acsh_test_syntax_input
echo "fi" >> /tmp/acsh_test_syntax_input
echo "exit" >> /tmp/acsh_test_syntax_input
./acsh < /tmp/acsh_test_syntax_input > /tmp/acsh_test_syntax_output 2>&1
grep -q "usage:" /tmp/acsh_test_syntax_output
if test $? -eq 0
then
    check_eq "syntax error includes a usage hint" "yes" "yes"
else
    check_eq "syntax error includes a usage hint" "yes" "no"
fi
rm -f /tmp/acsh_test_syntax_input /tmp/acsh_test_syntax_output

echo ""
echo "[1.5] [[ ]] extended test (not present in ash at all)"
if [[ hello = hello ]]
then
    check_eq "[[ ]] basic equality" "yes" "yes"
else
    check_eq "[[ ]] basic equality" "yes" "no"
fi
if [[ hello == hello ]]
then
    check_eq "[[ ]] == as alias for =" "yes" "yes"
else
    check_eq "[[ ]] == as alias for =" "yes" "no"
fi

echo ""
echo "[1.6] type distinguishes function / builtin / PATH"
greet_for_type_test() {
    echo hi
}
type greet_for_type_test > /tmp/acsh_test_type_output 2>&1
grep -q "shell function" /tmp/acsh_test_type_output
if test $? -eq 0
then
    check_eq "type reports a shell function" "yes" "yes"
else
    check_eq "type reports a shell function" "yes" "no"
fi
type cd > /tmp/acsh_test_type_output2 2>&1
grep -q "shell builtin" /tmp/acsh_test_type_output2
if test $? -eq 0
then
    check_eq "type reports a builtin" "yes" "yes"
else
    check_eq "type reports a builtin" "yes" "no"
fi
type ls > /tmp/acsh_test_type_output3 2>&1
grep -q "/ls" /tmp/acsh_test_type_output3
if test $? -eq 0
then
    check_eq "type reports a PATH location" "yes" "yes"
else
    check_eq "type reports a PATH location" "yes" "no"
fi
rm -f /tmp/acsh_test_type_output /tmp/acsh_test_type_output2 /tmp/acsh_test_type_output3

echo ""
echo "--- Section 2: core POSIX/ash-parity features ---"

echo ""
echo "[2.1] Pipes and redirection"
echo "pipeline" > /tmp/acsh_test_pipe_in
RESULT=$(cat /tmp/acsh_test_pipe_in | grep pipeline)
check_eq "pipe through grep" "pipeline" "$RESULT"
rm -f /tmp/acsh_test_pipe_in

echo "redirtest" > /tmp/acsh_test_redir.txt
RESULT=$(cat /tmp/acsh_test_redir.txt)
check_eq "output redirection" "redirtest" "$RESULT"
rm -f /tmp/acsh_test_redir.txt

ls /nonexistent_acsh_probe > /tmp/acsh_test_merge.txt 2>&1
if test -s /tmp/acsh_test_merge.txt
then
    check_eq "2>&1 merges stderr into the redirected stdout" "yes" "yes"
else
    check_eq "2>&1 merges stderr into the redirected stdout" "yes" "no"
fi
rm -f /tmp/acsh_test_merge.txt

ls /nonexistent_acsh_probe 2> /tmp/acsh_test_errfile.txt
if test -s /tmp/acsh_test_errfile.txt
then
    check_eq "2>file captures stderr to a file" "yes" "yes"
else
    check_eq "2>file captures stderr to a file" "yes" "no"
fi
rm -f /tmp/acsh_test_errfile.txt

echo ""
echo "[2.2] Quoting"
RESULT=$(echo "hello world")
check_eq "double-quoted word stays together" "hello world" "$RESULT"
RESULT=$(echo 'literal $HOME')
check_eq "single quotes block expansion" 'literal $HOME' "$RESULT"
RESULT=$(echo "escaped \$HOME")
check_eq "backslash-dollar in double quotes stays literal" 'escaped $HOME' "$RESULT"

echo ""
echo "[2.3] Variable expansion and exit status"
export ACSH_VAR_TEST=expanded
RESULT=$(echo $ACSH_VAR_TEST)
check_eq "basic variable expansion" "expanded" "$RESULT"

false
check_eq "exit status after false" "1" "$?"

true
check_eq "exit status after true" "0" "$?"

echo ""
echo "[2.4] Command substitution"
RESULT=$(echo nested)
check_eq "basic \$(...) substitution" "nested" "$RESULT"
RESULT=$(echo outer $(echo inner))
check_eq "nested \$(...) substitution" "outer inner" "$RESULT"
RESULT=`echo backtick-form`
check_eq "backtick substitution" "backtick-form" "$RESULT"

echo ""
echo "[2.5] Chaining: && || ;"
RESULT=$(true && echo and-ran)
check_eq "&& runs after success" "and-ran" "$RESULT"
RESULT=$(false && echo should-not-appear)
check_eq "&& skipped after failure" "" "$RESULT"
RESULT=$(false || echo or-ran)
check_eq "|| runs after failure" "or-ran" "$RESULT"

echo ""
echo "[2.6] Globbing"
mkdir -p /tmp/acsh_test_glob_dir
touch /tmp/acsh_test_glob_dir/a.txt
touch /tmp/acsh_test_glob_dir/b.txt
RESULT=$(ls /tmp/acsh_test_glob_dir/*.txt | wc -l)
check_eq "glob expands *.txt to two files" "2" "$RESULT"
rm -rf /tmp/acsh_test_glob_dir

echo ""
echo "[2.7] if / elif / else"
if true
then
    RESULT=if-branch
else
    RESULT=else-branch
fi
check_eq "if takes true branch" "if-branch" "$RESULT"

if false
then
    RESULT=if-branch
elif true
then
    RESULT=elif-branch
else
    RESULT=else-branch
fi
check_eq "elif taken when if is false" "elif-branch" "$RESULT"

echo ""
echo "[2.8] while / until loops"
rm -f /tmp/acsh_test_while_marker
while test ! -f /tmp/acsh_test_while_marker
do
    touch /tmp/acsh_test_while_marker
    RESULT=while-ran
done
check_eq "while loop body ran once" "while-ran" "$RESULT"
rm -f /tmp/acsh_test_while_marker

rm -f /tmp/acsh_test_until_marker
until test -f /tmp/acsh_test_until_marker
do
    touch /tmp/acsh_test_until_marker
    RESULT=until-ran
done
check_eq "until loop body ran once" "until-ran" "$RESULT"
rm -f /tmp/acsh_test_until_marker

echo ""
echo "[2.9] for loops, including word-splitting a substitution"
RESULT=""
for word in a b c
do
    RESULT="$RESULT$word"
done
check_eq "for loop over literal word list" "abc" "$RESULT"

RESULT=""
for word in $(echo x y z)
do
    RESULT="$RESULT$word"
done
check_eq "for loop word-splits a command substitution" "xyz" "$RESULT"

echo ""
echo "[2.10] case statement with glob patterns"
TESTFILE=hello.txt
case $TESTFILE in
    *.txt) RESULT=text-match ;;
    *.c) RESULT=code-match ;;
    *) RESULT=no-match ;;
esac
check_eq "case matches glob pattern" "text-match" "$RESULT"

echo ""
echo "[2.11] break and continue"
RESULT=""
for x in a b c d e
do
    if test "$x" = "c"
    then
        break
    fi
    RESULT="$RESULT$x"
done
check_eq "break stops the loop early" "ab" "$RESULT"

RESULT=""
for x in a b c d e
do
    if test "$x" = "c"
    then
        continue
    fi
    RESULT="$RESULT$x"
done
check_eq "continue skips one iteration" "abde" "$RESULT"

echo ""
echo "[2.12] Functions and positional parameters"
greet_fn() {
    echo "hello $1, arg count is $#"
}
RESULT=$(greet_fn World)
check_eq "function with positional parameter and count" "hello World, arg count is 1" "$RESULT"

sum_args() {
    echo "got: $@"
}
RESULT=$(sum_args one two three)
check_eq "function all-arguments expansion" "got: one two three" "$RESULT"

echo ""
echo "[2.13] test / [ builtin operators"
if test 5 -lt 10
then
    check_eq "test -lt" "yes" "yes"
else
    check_eq "test -lt" "yes" "no"
fi
if [ -z "" ]
then
    check_eq "[ -z on empty string ]" "yes" "yes"
else
    check_eq "[ -z on empty string ]" "yes" "no"
fi

echo ""
echo "[2.14] read builtin"
echo "readvalue" > /tmp/acsh_test_read_input
read ACSH_READ_RESULT < /tmp/acsh_test_read_input
check_eq "read populates a variable from stdin" "readvalue" "$ACSH_READ_RESULT"
rm -f /tmp/acsh_test_read_input

echo ""
echo "[2.15] local variables are function-scoped"
export ACSH_LOCAL_PROBE=outer-value
scope_test_fn() {
    local ACSH_LOCAL_PROBE=inner-value
}
scope_test_fn
check_eq "local variable restored after function returns" "outer-value" "$ACSH_LOCAL_PROBE"

echo ""
echo "[2.16] shift builtin"
shift_test_fn() {
    shift
    echo "$1"
}
RESULT=$(shift_test_fn a b c)
check_eq "shift advances positional parameters" "b" "$RESULT"

echo ""
echo "[2.17] export / unset"
export ACSH_UNSET_PROBE=present
unset ACSH_UNSET_PROBE
RESULT=$ACSH_UNSET_PROBE
check_eq "unset clears a variable" "" "$RESULT"

echo ""
echo "[2.18] eval"
RESULT=$(eval echo eval-worked)
check_eq "eval runs a constructed command" "eval-worked" "$RESULT"

echo ""
echo "[2.19] alias and unalias"
alias acsh_test_alias_cmd="echo aliased-output"
RESULT=$(acsh_test_alias_cmd)
check_eq "user-defined alias expands" "aliased-output" "$RESULT"
unalias acsh_test_alias_cmd

echo ""
echo "[2.20] Builtins work as pipeline stages"
RESULT=$(alias | grep "^alias ll=" | wc -l)
check_eq "builtin (alias) piped into grep" "1" "$RESULT"

echo ""
echo "=================================================="
echo " Summary"
echo "=================================================="
PASS_COUNT=$(wc -l < /tmp/acsh_test_pass_count)
FAIL_COUNT=$(wc -l < /tmp/acsh_test_fail_count)
echo "PASS: $PASS_COUNT"
echo "FAIL: $FAIL_COUNT"
rm -f /tmp/acsh_test_pass_count /tmp/acsh_test_fail_count

if test "$FAIL_COUNT" = "0"
then
    echo "All checks passed."
else
    echo "Some checks failed -- see FAIL lines above for details."
fi
