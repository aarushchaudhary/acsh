# Field splitting on $IFS, "$@" and "$*", empty fields.
x="1  2"; printf "[%s]" $x "$x"; echo
IFS=:; y=a:b::c; for i in $y; do printf "[%s]" "$i"; done; echo
y=:lead; for i in $y; do printf "[%s]" "$i"; done; echo
y="trail:"; for i in $y; do printf "[%s]" "$i"; done; echo
IFS=' :'; y=" a : b  c "; for i in $y; do printf "[%s]" "$i"; done; echo
unset IFS
set -- "a b" c ""
for i in "$@"; do printf "[%s]" "$i"; done; echo
for i in $@; do printf "[%s]" "$i"; done; echo
for i in "$*"; do printf "[%s]" "$i"; done; echo
IFS=-; echo "$*"; unset IFS
set --; for i in "$@"; do echo none; done; echo "count=$#"
e=; printf "[%s]" $e "$e" '' x; echo
literal=a:b; IFS=:; printf "[%s]" a:b $literal; echo; unset IFS
echo $(printf 'x\ny\n') "$(printf 'x\ny\n')"
