# ash's bash-compatible expansions: substrings, replacement, $RANDOM, $LINENO.
x=hello
echo ${x:1:3} ${x:1} ${x: -2} ${x:(-3):2} ${x:0:0}x ${x:10} ${x:1:-1}
echo ${x/l/L} ${x//l/L} ${x/l} ${x//l} ${x/x/y}
f=a.b.c; echo ${f//./-} "${f/./ }"
p="*"; y="a*b"; echo ${y/"$p"/S} ${y/$p/S}
s=abc; i=1; echo ${s:i:1} ${s:$((i+1))}
set -- a b c d; echo ${@:2} ${@:2:1} ${*:3}
echo $RANDOM | grep -cE '^[0-9]+$'
RANDOM=5; a=$RANDOM; RANDOM=5; b=$RANDOM; test $a = $b && echo seeded-same
test $RANDOM != $RANDOM && echo varies
echo "line $LINENO"
g() {
    echo "in g line $LINENO"
}
g
