#!./acsh
#
# test_acsh.sh -- exercises every "beyond ash" feature claimed in
# README.md, plus the core features those improvements depend on.
#
# This file is itself an acsh script and only uses syntax acsh
# actually supports (see README.md "Known limitations": no arithmetic
# expansion, etc).
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
echo "[1.1] Default aliases in an interactive session, none in scripts"
echo "alias" > /tmp/acsh_test_alias_input
./acsh -i < /tmp/acsh_test_alias_input > /tmp/acsh_test_alias_check 2>&1
if grep -q "ll='ls" /tmp/acsh_test_alias_check
then
    check_eq "default alias ll is defined at startup" "yes" "yes"
else
    check_eq "default alias ll is defined at startup" "yes" "no"
fi
if grep -q "la='ls" /tmp/acsh_test_alias_check
then
    check_eq "default alias la is defined at startup" "yes" "yes"
else
    check_eq "default alias la is defined at startup" "yes" "no"
fi
RESULT=$(alias | wc -l)
check_eq "no default aliases inside a script" "0" "$RESULT"
rm -f /tmp/acsh_test_alias_input /tmp/acsh_test_alias_check

echo ""
echo "[1.2] Persistent history (~/.acsh_history gets written to)"
export HOME=/tmp/acsh_test_home
mkdir -p $HOME
echo "echo history-probe-line" > /tmp/acsh_test_hist_input
echo "exit" >> /tmp/acsh_test_hist_input
./acsh -i < /tmp/acsh_test_hist_input > /dev/null 2>&1
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
RESULT=$(ls -l $HOME/.acsh_history | cut -c1-10)
check_eq "history file is readable only by its owner" "-rw-------" "$RESULT"
rm -rf $HOME /tmp/acsh_test_hist_input

echo ""
echo "[1.3] Non-empty startup file support (~/.acshrc is read)"
export HOME=/tmp/acsh_test_home2
mkdir -p $HOME
echo "export ACSH_RC_PROBE=loaded" > $HOME/.acshrc
echo "echo rc-value: \$ACSH_RC_PROBE" > /tmp/acsh_test_rc_input
echo "exit" >> /tmp/acsh_test_rc_input
./acsh -i < /tmp/acsh_test_rc_input > /tmp/acsh_test_rc_output 2>&1
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
./acsh -i < /tmp/acsh_test_syntax_input > /tmp/acsh_test_syntax_output 2>&1
grep -q "usage:" /tmp/acsh_test_syntax_output
if test $? -eq 0
then
    check_eq "syntax error includes a usage hint" "yes" "yes"
else
    check_eq "syntax error includes a usage hint" "yes" "no"
fi
rm -f /tmp/acsh_test_syntax_input /tmp/acsh_test_syntax_output

echo ""
echo "[1.5] [[ ]] extended test"
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
alias acsh_pipe_alias="echo x"
RESULT=$(alias | grep "^acsh_pipe_alias=" | wc -l)
check_eq "builtin (alias) piped into grep" "1" "$RESULT"

echo ""
echo "[2.21] fg returns the job's exit status"
RESULT=$(printf 'sh -c "sleep 0.1; exit 3" > /dev/null &\nfg > /dev/null\necho "fg=$?"\n' | ./acsh -i 2>/dev/null)
check_eq "fg reports the job's real status" "fg=3" "$RESULT"
sleep 0.1 &
fg 2>/dev/null
check_eq "fg in a script (no job control) fails with 2, as in ash" "2" "$?"

echo ""
echo "[2.22] Invocation: -c with arguments, piped stdin without a prompt"
RESULT=$(./acsh -c 'echo "$0 $1 $#"' name one two)
check_eq "-c sets \$0 and the arguments" "name one 2" "$RESULT"
RESULT=$(echo 'echo piped' | ./acsh 2>&1)
check_eq "piped stdin runs without a prompt" "piped" "$RESULT"
RESULT=$(echo 'echo "$1 $#"' | ./acsh -s x y)
check_eq "-s takes arguments for stdin commands" "x 2" "$RESULT"
./acsh /nonexistent/acsh-script 2>/dev/null
check_eq "a missing script exits 2, as in ash" "2" "$?"

echo ""
echo "[2.23] Login shells read ~/.profile"
export HOME=/tmp/acsh_test_login_home
mkdir -p $HOME
echo "ACSH_PROFILE_PROBE=from-profile" > $HOME/.profile
RESULT=$(./acsh -lc 'echo $ACSH_PROFILE_PROBE' 2>/dev/null)
check_eq "-l reads ~/.profile" "from-profile" "$RESULT"
RESULT=$(./acsh -c 'echo "[$ACSH_PROFILE_PROBE]"')
check_eq "a non-login shell does not" "[]" "$RESULT"
rm -rf $HOME

echo ""
echo "[2.24] [[ ]] extensions beyond ash: grouping and string < >"
if [[ a = a && ( b = c || d = d ) ]]
then
    check_eq "[[ ]] groups with parentheses" "yes" "yes"
else
    check_eq "[[ ]] groups with parentheses" "yes" "no"
fi
if [[ abc < abd ]]
then
    check_eq "[[ ]] compares strings with <" "yes" "yes"
else
    check_eq "[[ ]] compares strings with <" "yes" "no"
fi

echo ""
echo "[2.25] Replacement anchors and \"\${@:n:m}\" (beyond ash)"
x=hello
check_eq "\${x/#h/H} anchors at the start" "Hello" "${x/#h/H}"
check_eq "\${x/%o/O} anchors at the end" "hellO" "${x/%o/O}"
slice_args() { for a in "${@:2:2}"; do printf "[%s]" "$a"; done; }
RESULT=$(slice_args a b c d)
check_eq "\"\${@:2:2}\" keeps both words" "[b][c]" "$RESULT"

echo ""
echo "[2.26] Errors name the script and line"
printf 'echo ok\nno_such_cmd_probe\n' > /tmp/acsh_test_err.sh
RESULT=$(./acsh /tmp/acsh_test_err.sh 2>&1 | grep -c "acsh_test_err.sh: line 2: no_such_cmd_probe: command not found")
check_eq "runtime error carries file and line" "1" "$RESULT"
printf 'echo ok\nif true\necho oops\nfi\n' > /tmp/acsh_test_err.sh
RESULT=$(./acsh /tmp/acsh_test_err.sh 2>&1 | grep -c "acsh_test_err.sh: line 4: syntax error: expected 'then'")
check_eq "syntax error carries file and line" "1" "$RESULT"
rm -f /tmp/acsh_test_err.sh

echo ""
echo "[2.27] Syntax beyond ash: ;& fall-through, &>>, bash escapes in \$'...'"
RESULT=$(case a in a) printf 1;& b) printf 2;; c) printf 3;; esac)
check_eq ";& runs the next clause too (POSIX.1-2024)" "12" "$RESULT"
rm -f /tmp/acsh_test_amp.out
echo one &>> /tmp/acsh_test_amp.out
ls /nonexistent-acsh-probe &>> /tmp/acsh_test_amp.out
RESULT=$(wc -l < /tmp/acsh_test_amp.out | tr -d ' ')
check_eq "&>> appends stdout and stderr" "2" "$RESULT"
rm -f /tmp/acsh_test_amp.out
RESULT=$(printf '%s' $'é\e' | od -An -tx1 | tr -d ' \n')
check_eq "\$'\\u00e9\\e' gives UTF-8 and ESC" "c3a91b" "$RESULT"
RESULT=$(printf '%s' $'a\0b')
check_eq "\$'...' ends at \\0, as in bash" "a" "$RESULT"
RESULT=$(./acsh -c 'trap "echo hi" INT; trap -p INT; trap -p TERM')
check_eq "trap -p SIGNAL lists that trap (POSIX.1-2024)" "trap -- 'echo hi' INT" "$RESULT"

echo ""
echo "[2.28] \$ENV is run by interactive shells"
printf 'X_FROM_ENV=yes\n' > /tmp/acsh_test_env.sh
RESULT=$(echo 'echo "[$X_FROM_ENV]"' | ENV=/tmp/acsh_test_env.sh ./acsh -i 2>/dev/null)
check_eq "interactive shell runs \$ENV" "[yes]" "$RESULT"
RESULT=$(echo 'echo "[$X_FROM_ENV]"' | ENV=/tmp/acsh_test_env.sh ./acsh 2>/dev/null)
check_eq "a non-interactive shell does not" "[]" "$RESULT"
rm -f /tmp/acsh_test_env.sh
RESULT=$(env -i ./acsh -c 'echo "$PATH"')
check_eq "an unset PATH gets ash's default" "/sbin:/usr/sbin:/bin:/usr/bin" "$RESULT"


echo ""
echo "[2.29] POSIX signal rules for scripts"
RESULT=$( (trap '' INT; ./acsh -c 'trap "echo caught" INT; kill -INT $$; echo after') 2>&1)
check_eq "a signal ignored on entry cannot be trapped" "after" "$RESULT"
RESULT=$(./acsh -c 'sleep 2 & p=$!; sleep 0.3; kill -INT $p; wait $p; echo "st=$?"')
check_eq "a background job in a script ignores SIGINT" "st=0" "$RESULT"
RESULT=$(./acsh -c 'trap "" INT; sh -c "kill -INT \$\$; echo survived"')
check_eq "trap '' INT is inherited by commands" "survived" "$RESULT"

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
    exit 1
fi
