# Parameter expansion operators.
x=; unset y z
echo "[${x:-d}] [${x-d}] [${y-d}] [${y:-d}]"
echo "[${y=new}] $y [${z:=made}] $z"
echo "[${x:+alt}] [${y:+alt}] [${unset_var+alt}]"
echo "[${#y}] [${#unset_var}] [${#}]"
f=a.b.c
echo ${f%.*} ${f%%.*} ${f#*.} ${f##*.}
p=/usr/local/bin/tool.tar.gz
echo "${p##*/} ${p%/*} ${p%.gz} ${p#/usr}"
q='*'; echo "${f#"a."}" "${f%"$q"}"
w="two words"; echo ${w:-x} "${w:-x}"
set -- one "two three"
echo "${1:-a} ${2%% *} ${#1} ${@:+set}"
dflt() { echo "[${1:-default arg}]"; }; dflt; dflt given
