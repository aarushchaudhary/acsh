# Tilde, quoting with globs, command substitution forms.
HOME=/home/test-user
echo ~ ~/x "~" '~' x~ ~root
v=~/inside:~/second; echo "$v"
touch g1.txt g2.txt
echo g*.txt "g*.txt" 'g*.txt' g\*.txt
pat='g*.txt'; echo $pat "$pat"
echo "$(echo "inner $(echo deep)")" `echo back \`echo nested\`` "a\$b"
echo "x$(true)y" "$(echo)" end
set -f; echo g*.txt; set +f; echo g*.txt
