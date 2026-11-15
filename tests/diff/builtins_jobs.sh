# wait, kill, true/false/:, let, special-builtin assignments, cd -.
sleep 0.1 & p=$!; wait $p; echo "wait pid st=$?"
(exit 3) & wait $!; echo "wait st=$?"
sleep 0.1 & wait; echo "wait all st=$?"
wait 99999; echo "unknown pid st=$?"
sleep 5 & kill %1; wait %1; echo "kill job st=$?"
sleep 5 & kill -s TERM $!; wait $!; echo "TERM st=$?"
sleep 5 & kill -9 $!; wait $!; echo "KILL st=$?"
kill -l 15; kill -l 143
: ignored args; echo "colon st=$?"; true; echo "true $?"; false; echo "false $?"
let x=1+2 y=x*3; echo "let $x $y st=$?"; let 0; echo "let0 st=$?"
z=9 :; echo "after special builtin [$z]"
cd /tmp; cd /; cd - >/dev/null; echo "PWD=$PWD OLDPWD=$OLDPWD"
cd /nonexistent 2>/dev/null; echo "cd failure st=$?"
type : echo cd
