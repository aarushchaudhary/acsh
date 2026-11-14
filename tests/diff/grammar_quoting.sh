# Quoting and escapes.
echo 'single $HOME' "double \$HOME" plain\ space
echo "a \" b" 'it'"'"'s'
echo "tab	inside" \* "*" '*'
touch q1.txt q2.txt
echo q*.txt "q*.txt" 'q*.txt' q\*.txt
x='  spaced  '; echo "[$x]"
echo "$(echo "nested quotes")"
echo `echo backtick`
echo "multi
line"
empty=""; for w in a "" b; do echo "[$w]"; done
set_x() { echo "$#"; }; set_x "" ''
