# set -e and its exceptions.
set -e
if false; then true; fi
false || true
! true
false && true
echo survived
while false; do true; done
f() { false; echo not-in-f; }
if f; then true; fi
echo "f in a condition does not exit"
(false) || echo "subshell failure handled"
f
echo not-reached
