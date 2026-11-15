# CVE-2026-38755 is a heap overflow in busybox ash's evalcommand(); no
# input was published. These stress the same code: many assignments,
# many redirections and many arguments on one command.
assigns=$(seq 1 5000 | sed 's/.*/v&=&/' | tr '\n' ' ')
eval "$assigns true"; echo "prefix assignments ok"
eval "$assigns env" | grep -c '^v' >/dev/null; echo "prefix assignments to a program ok"
redirs=$(seq 1 3000 | sed 's|.*|>/dev/null|' | tr '\n' ' ')
eval "echo hidden $redirs"; echo "many redirections ok"
eval "x=1 y=2 $redirs :"; echo "assignments and redirections ok"
set -- $(seq 1 100000); echo "arguments: $#"
printf '%s\n' "$@" | tail -1
f() { echo "function args: $#"; }; f "$@"
eval "x$(seq 1 2000 | tr -d '\n')=1"; echo "long name ok"
exec 3>/dev/null 4>&3 5>&4 6>&5 7>&6 8>&7 9>&8; echo "fds ok"
echo survived
