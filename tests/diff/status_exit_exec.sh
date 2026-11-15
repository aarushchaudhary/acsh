# $? in assignments, exit statuses through EXIT traps, and exec passing
# the shell's exported variables: what every autoconf configure relies on.
# (Run by tests/run_diff.sh, which names the shell under test in
# SHELL_UNDER_TEST.)
false; rc=$?; echo "rc=$rc"
x=$(exit 3); echo "assignment with a substitution: $?"
false; x=plain; echo "plain assignment: $?"
false; $empty_var; echo "empty command: $?"
./acsh_absent 2>/dev/null; rc=$?; echo "not found: $rc"
$SHELL_UNDER_TEST -c 'trap "echo in-trap=\$?" EXIT; exit 3'; echo "exit 3 through a trap: $?"
$SHELL_UNDER_TEST -c 'trap "true; exit" EXIT; exit 4'; echo "bare exit in the trap keeps 4: $?"
$SHELL_UNDER_TEST -c 'trap "true" EXIT; false'; echo "trap does not change the status: $?"
$SHELL_UNDER_TEST -c 'trap "exit 6" EXIT; exit 4'; echo "exit in the trap wins: $?"
$SHELL_UNDER_TEST -c 'f() { exit 1; }; t() { s=; true; exit $s; }; trap "t \$?" 0; f'; echo "configure-style trap: $?"
export EXPORTED_FOR_EXEC=yes
$SHELL_UNDER_TEST -c 'exec sh -c "echo exec sees: \$EXPORTED_FOR_EXEC"'
$SHELL_UNDER_TEST -c 'X=1; export X; exec sh -c "echo exported then exec: \$X"'
$SHELL_UNDER_TEST -c 'Y=2 exec sh -c "echo prefix then exec: \$Y"'
