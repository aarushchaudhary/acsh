# Shell variables vs exported ones, one-command assignments, readonly.
x=1; sh -c 'echo "child sees [$x]"'; export x; sh -c 'echo "child sees [$x]"'
p=5 true; echo "after regular builtin [$p]"
f() { echo "in f [$q]"; }; q=7 f; echo "after function [$q]"
w=3 eval 'echo in-eval $w'; echo "after eval [$w]"
e=only-for-child sh -c 'echo "child got [$e]"'; echo "shell has [$e]"
export -p | grep ' x='
set -a; auto=1; sh -c 'echo "allexport [$auto]"'; set +a
v="it's"; set | grep '^v='
unset -v v; echo "unset [$v]"
g() { echo g; }; unset -f g; g 2>/dev/null || echo "function removed"
readonly r=1
echo "readonly is [$r]"
r=2
echo not-reached
