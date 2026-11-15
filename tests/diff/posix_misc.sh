# POSIX details: exit statuses 126/127, bracket expressions and character
# classes, `for x do`, empty IFS, aliases ending in a blank, assignments
# before special builtins, true/false, ulimit, hash.
printf 'echo hi\n' > notexec.sh; chmod -x notexec.sh
./notexec.sh 2>/dev/null; echo "not executable: $?"
no_such_command_posix 2>/dev/null; echo "not found: $?"
for c in b 5 _ ' '; do case $c in [[:alpha:]]) echo "$c alpha";; [[:digit:]]) echo "$c digit";; [!a-z0-9]) echo "[$c] other";; esac; done
touch fa1 fb2 fc3; echo f[[:alpha:]][[:digit:]] f[!a]? f[ab]*
set -- one two; for x do echo "arg $x"; done
IFS=; v="a b"; set -- $v; echo "empty IFS: $#"; unset IFS
alias say='echo '
alias who=world
say who
x=kept :; echo "after special builtin: $x"
y=temp true; echo "after regular builtin: [${y-unset}]"
true; echo "true $?"; false; echo "false $?"
ulimit -n >/dev/null; echo "ulimit $?"
hash ls 2>/dev/null; hash -r; echo "hash $?"
f() { echo "f sees $1"; }; set -- a b c d e f g h i j; echo "${10}"; f "${10}"
