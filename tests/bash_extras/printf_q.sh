# printf %q: every line must print what bash prints.
for s in "" "abc" "a b" "it's" "a\"b" "\$x" "a*b?" "~home" "a~b" "x=y" "#c" "a#c" "a,b" "{a}" "tab	x" "nl
x" "é" "back\\slash" "semi;amp&" "a%b" "a^b" "a@b:c+d" "-opt" "!bang" "(paren)" "[x]" "a|b<c>d" "esc"$'\e' $'q\'\\'"" ; do printf "%q|" "$s"; done; echo
printf "%q %q\n" a "b c"; printf "[%10q][%-6q]\n" ab cd
eval "x=$(printf %q "a b \$c 'd'")"; echo "$x"
