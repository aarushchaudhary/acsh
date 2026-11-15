# More of ash's bash-compatible syntax: function keyword, $'...', &>,
# process substitution, base#n arithmetic.
function f { echo "f got $1"; }
f one
function g() { echo g; }
g
echo $'tab:\t|nl:\n|' | od -c | head -2
echo $'\x41\101' $'it\'s' "$'kept'"
x=$(echo $'in \'subst\''); echo "$x"
(echo noise; echo noise >&2; exit 3) &> /dev/null; echo "status $?"
{ echo out; echo err >&2; } &> amp_test.out; cat amp_test.out; rm -f amp_test.out
cat <(echo procsub)
cat <(echo 1) <(echo 2)
while read l; do echo "[$l]"; done < <(printf 'r1\nr2\n')
echo upper > >(tr a-z A-Z); sleep 0.2
h() { cat <(echo in-function); }; h
echo $((16#ff)) $((2#1010)) $((8#17)) $((36#z)) $((64#_))
echo $((2**10)) $((2**0)) $((3**3**2)) $((-2**2)) $((x=2, x**3)) $(( (1,5)*2 ))
for v in OPTIND IFS PS2 PS4 HOSTNAME PPID; do eval "s=\${$v+set}"; echo "$v=$s"; done
printf '[%s]\n' "$IFS$PS2$PS4$OPTIND"
x=3+4; echo $((x*2)); y=abc; echo $((y+1)); abc=5; echo $((y+1)); x=abc; echo $((x=2))
LC_ALL=C.UTF-8; x=é€; echo "utf8 length ${#x}"; LC_ALL=C; echo "byte length ${#x}"; unset LC_ALL
