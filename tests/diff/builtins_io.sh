# echo and printf.
echo -n a; echo b; echo -e "x\ty|\101\0102\x41"; echo "x\ty"; echo -E "z\tz"; echo -- -n; echo -nx foo
echo -e "stop\chere"; echo
printf "%s-%d-%x-%o-%c-%5s|%-5s|%.2s|%05d|%%\n" str 42 255 8 char ab cd xyz 7
printf "%s\n" a b c; printf "%d %s\n" 1; printf "%b\n" "a\tb"; printf "x\ty\n"
printf "%5.1f|%e|%u|%i|%X\n" 3.14159 1000 7 9 255; printf "\101\n"
printf "%*d|%-*s|\n" 5 42 4 ab; printf "[%s]\n"; printf "%d\n" "'A"
printf "%d\n" abc; echo "invalid number st=$?"
