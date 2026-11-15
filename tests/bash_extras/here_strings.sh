# <<< here-strings: every line must print what bash prints.
cat <<< hello
x="a  b"; cat <<< $x; cat <<< "$x"
read -r a b <<< "one two three"; echo "[$a][$b]"
wc -l <<< ''
tr a-z A-Z <<<'single $x'
cat 3<<< "on fd 3" <&3
while read l; do echo "<$l>"; done <<< "$(printf 'p\nq')"
f() { cat; }; f <<< "func input"
cat <<< *.nonexistent
