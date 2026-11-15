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
RESULT=$(echo "$EPOCHSECONDS" | grep -c '^[0-9][0-9]*$')
check_eq "\$EPOCHSECONDS is a number (busybox 1.37 has it, 1.36 does not)" "1" "$RESULT"
RESULT=$(echo "$EPOCHREALTIME" | grep -c '^[0-9][0-9]*\.[0-9]\{6\}$')
check_eq "\$EPOCHREALTIME has microseconds" "1" "$RESULT"


echo ""
echo "[2.29] POSIX signal rules for scripts"
RESULT=$( (trap '' INT; ./acsh -c 'trap "echo caught" INT; kill -INT $$; echo after') 2>&1)
check_eq "a signal ignored on entry cannot be trapped" "after" "$RESULT"
RESULT=$(./acsh -c 'sleep 2 & p=$!; sleep 0.3; kill -INT $p; wait $p; echo "st=$?"')
check_eq "a background job in a script ignores SIGINT" "st=0" "$RESULT"
RESULT=$(./acsh -c 'trap "" INT; sh -c "kill -INT \$\$; echo survived"')
check_eq "trap '' INT is inherited by commands" "survived" "$RESULT"

echo ""
echo "[3.1] No crash on hostile input (deep nesting, huge values, overflow)"
deep() { # deep NAME OPEN CLOSE MIDDLE: a 100,000-deep script, run; prints the exit status
    o=$(head -c 100000 /dev/zero | tr '\0' "$2")
    c=$(head -c 100000 /dev/zero | tr '\0' "$3")
    printf '%s%s%s\n' "$o" "$4" "$c" > /tmp/acsh_deep_$1.sh
    ./acsh /tmp/acsh_deep_$1.sh >/dev/null 2>&1
    echo $?
    rm -f /tmp/acsh_deep_$1.sh
}
check_eq "100,000 nested ( ) is a syntax error, not a crash" "2" "$(deep paren '(' ')' 'echo x')"
check_eq "100,000 nested \$( ) is a syntax error, not a crash" "2" "$(deep subst '$' ')' '(echo x')"
check_eq "100,000 nested arithmetic ( ) is an error, not a crash" "2" "$(deep arith '(' ')' 'echo $((1')"
printf 'true' > /tmp/acsh_deep_ao.sh
head -c 100000 /dev/zero | tr '\0' '&' | sed 's/&&/ \&\& true/g' >> /tmp/acsh_deep_ao.sh
printf '; echo andor-ok\n' >> /tmp/acsh_deep_ao.sh
check_eq "a 50,000-long && list runs" "andor-ok" "$(./acsh /tmp/acsh_deep_ao.sh 2>&1)"
rm -f /tmp/acsh_deep_ao.sh
./acsh -c 'e="eval \"\$e\""; eval "$e"' 2>/dev/null
check_eq "endless eval recursion ends the script with status 2" "2" "$?"
RESULT=$(./acsh -c 'x=$(head -c 1048576 /dev/zero | tr "\0" a); z=${x//a/bb}; IFS=a; set -- $x; echo ${#z} $#')
check_eq "a 1 MB value: replace all and split, quickly" "2097152 1048576" "$RESULT"
RESULT=$(./acsh -c 'echo $(( (-9223372036854775807-1) / -1 )) $((9223372036854775807+1)) $((3**9223372036854775807))' 2>&1)
check_eq "arithmetic overflow wraps (no SIGFPE)" "-9223372036854775808 -9223372036854775808 -6148914691236517205" "$RESULT"
# CVE-2022-48174 was an ash arithmetic overflow; these are its kind of input
printf 'echo $((%s1))\n' "$(head -c 200000 /dev/zero | tr '\0' '!')" > /tmp/acsh_cve.sh
./acsh /tmp/acsh_cve.sh >/dev/null 2>&1
check_eq "200,000 nested ! in arithmetic: an error, not a crash" "2" "$?"
printf 'echo $((%s1))\n' "$(head -c 100000 /dev/zero | tr '\0' 'x' | sed 's/x/a=/g')" > /tmp/acsh_cve.sh
./acsh /tmp/acsh_cve.sh >/dev/null 2>&1
check_eq "a 100,000-long a=a=...=1 chain: an error, not a crash (ash segfaults)" "2" "$?"
printf 'x="%s1"; echo $((x))\n' "$(head -c 100000 /dev/zero | tr '\0' 'x' | sed 's/x/1+/g')" > /tmp/acsh_cve.sh
check_eq "a 100,001-term sum inside a variable" "100001" "$(./acsh /tmp/acsh_cve.sh 2>&1)"
rm -f /tmp/acsh_cve.sh
RESULT=$(./acsh -c 'f() { f; }; f; echo "status $?"' 2>/dev/null)
check_eq "endless function recursion stops with an error" "status 1" "$RESULT"

echo ""
echo "[3.3] History: HISTCONTROL, HISTIGNORE, multi-line entries, locking, fc"
HDIR=/tmp/acsh_hist_test; rm -rf $HDIR; mkdir -p $HDIR
printf 'echo one\n mc-injected-cmd\nHISTIGNORE="ls*:&"\nls -a\necho two\necho two\nif true\nthen echo multi\nfi\n' | HOME=$HDIR ./acsh -i >/dev/null 2>&1
check_eq "a line starting with a space is not kept (default ignoreboth)" "0" "$(grep -c mc-injected $HDIR/.acsh_history)"
check_eq "HISTIGNORE patterns never reach the file" "0" "$(grep -c '^ls' $HDIR/.acsh_history)"
check_eq "repeated commands are kept once" "1" "$(grep -c '^echo two$' $HDIR/.acsh_history)"
check_eq "a multi-line command is one entry (backslash-continued in the file)" "if true\\" "$(grep '^if' $HDIR/.acsh_history)"
RESULT=$(printf 'history 2>/dev/null\n' | HOME=$HDIR ./acsh -i 2>/dev/null | grep -c 'then echo multi')
check_eq "history prints to stdout, multi-line entry intact after reload" "1" "$RESULT"
RESULT=$(printf 'echo hello world\nfc -s hello=bye\n' | HOME=$HDIR ./acsh -i 2>/dev/null | tail -1)
check_eq "fc -s OLD=NEW runs the edited command" "bye world" "$RESULT"
RESULT=$(printf 'echo a1\necho a2\nfc -ln -2\n' | HOME=$HDIR ./acsh -i 2>/dev/null | tail -2 | tr '\n' ' ')
check_eq "fc -l lists the last entries" "echo a1 echo a2 " "$RESULT"
rm -f $HDIR/.acsh_history
seq 1 300 | sed 's/^/echo A/' | HOME=$HDIR ./acsh -i >/dev/null 2>&1 &
seq 1 300 | sed 's/^/echo B/' | HOME=$HDIR ./acsh -i >/dev/null 2>&1
wait
check_eq "two shells writing at once leave 600 clean entries" "600 0" "$(wc -l < $HDIR/.acsh_history | tr -d ' ') $(grep -cv '^echo [AB][0-9]*$' $HDIR/.acsh_history)"
seq 1 30 | sed 's/^/echo n/' | HOME=$HDIR HISTSIZE=10 ./acsh -i >/dev/null 2>&1
check_eq "the file is trimmed to HISTSIZE" "10" "$(wc -l < $HDIR/.acsh_history | tr -d ' ')"
printf 'echo nofile\n' | HOME=$HDIR HISTFILE= ./acsh -i >/dev/null 2>&1
check_eq "HISTFILE= keeps no file" "0" "$(grep -c nofile $HDIR/.acsh_history)"
rm -rf $HDIR

echo ""
echo "[3.5] Bash-only syntax gets a message naming the feature and the alternative"
hint() { ./acsh -c "$1" 2>&1 | grep -c "$2"; }
check_eq "a=(1 2): arrays, use a space-separated list" "1" "$(hint 'a=(1 2)' 'arrays are a bash feature; use a space-separated list')"
check_eq "\${a[1]}: arrays" "1" "$(hint 'echo ${a[1]}' 'arrays are a bash feature')"
check_eq "a[1]=x: arrays" "1" "$(hint 'a[1]=x' 'arrays are a bash feature')"
check_eq "select: use while and read" "1" "$(hint 'select x in a b; do break; done' 'select is a bash feature; use a while loop with read')"
check_eq "declare: use NAME=value, export, readonly" "1" "$(hint 'declare -i n=1' 'declare is a bash builtin; use NAME=value')"
check_eq "mapfile: use while read" "1" "$(hint 'mapfile x </dev/null' 'mapfile is a bash builtin; use: while IFS= read -r line')"
check_eq "coproc: use a background job and mkfifo" "1" "$(hint 'coproc cat' 'coproc is a bash feature')"
check_eq "\${x^^}: use tr" "1" "$(hint 'x=a; echo ${x^^}' 'case changing is a bash feature; use tr')"
check_eq "\${!x}: use eval" "1" "$(hint 'x=y; echo ${!x}' 'indirection')"

echo ""
echo "[3.6] Bash extras: here-strings, brace expansion, printf %q (checked against bash)"
RESULT=$(read -r a b <<< "one two three"; echo "[$a][$b]")
check_eq "<<< feeds a word to read" "[one][two three]" "$RESULT"
x="a  b"
check_eq "<<< \"\$x\" keeps the spaces" "a  b" "$(cat <<< "$x")"
check_eq "{a,b,c} and nesting" "xay xby xcy" "$(echo x{a,{b,c}}y)"
check_eq "{1..10..3} and padded {01..03}" "1 4 7 10 01 02 03" "$(echo {1..10..3} {01..03})"
check_eq "{a..e} and {e..a..2}" "a b c d e e c a" "$(echo {a..e} {e..a..2})"
check_eq "quoted braces, {a} and {} stay literal" "{a,b} {a} {}" "$(echo "{a,b}" {a} {})"
check_eq "assignments are not brace-expanded" "{a,b}" "$(v={a,b}; echo $v)"
check_eq "set +B turns brace expansion off" "{a,b}" "$(set +B; echo {a,b})"
check_eq "printf %q quotes as bash does" "a\\ b it\\'s \\\$x '' \$'t\\tx'" "$(printf '%q ' 'a b' "it's" '$x' '' "$(printf 't\tx')" | sed 's/ $//')"
RESULT=$(eval "y=$(printf %q "a b \$c 'd'")"; echo "$y")
check_eq "printf %q output reads back unchanged" "a b \$c 'd'" "$RESULT"

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
