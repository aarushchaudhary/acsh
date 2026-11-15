# Brace expansion: every line must print what bash prints.
echo {a,b,c}
echo x{a,b}y
echo {a,b}{1,2}
echo x{a,{b,c}}y
echo {1..5} {5..1} {1..10..3} {10..1..4} {-2..2}
echo {01..10} {1..03} {-05..5..5}
echo {a..e} {e..a} {a..k..3}
echo {a} {} {a..} {1..x} {,a} a{,}b
echo "{a,b}" '{a,b}' \{a,b\} {a,"b c"}
x=1; echo ${x}{a,b} $(echo {p,q}) {$x,2}
for i in {1..3}; do printf %s $i; done; echo
v={a,b}; echo $v
echo {a,b}*.nonexist
echo a{b,c}{d,e}f {x..z}{1..2}
echo {1..2..0} {3..1..-1}
set +B; echo {a,b}; set -B; echo {a,b}
set +o braceexpand; echo {1..3}; set -o braceexpand
echo {Z..b}
eval "echo {$(seq -s, 1 300)}" | wc -w
eval "echo pre{$(seq -s, 1 300)}post" | tr ' ' '\n' | tail -1
