# CVE-2026-38754 is a heap overflow in busybox ash's IFS splitting
# (ifsbreakup); no input was published. These stress the same code.
big=$(head -c 200000 /dev/zero | tr '\0' a)
IFS=a; set -- $big; echo "fields: $#"
IFS='a '; set -- " $big "; set -- $1; echo "fields: $#"
IFS=$(printf ' \t\n*?[]'); x='a*b?c[d]e f'; set -- $x; echo "glob chars in IFS: $#"
IFS=$(head -c 5000 /dev/zero | tr '\0' x)y; x=1x2y3; set -- $x; echo "long IFS: $#"
IFS=$(printf '\303\251'); x=$(printf 'a\303\251b\303\251'); set -- $x; echo "multibyte IFS: $#"
IFS=: ; x=':::a::b:::'; set -- $x; echo "empty fields: $#"
IFS=; x='a b'; set -- $x "$@" $x; echo "empty IFS: $#"
unset IFS; set -- $(printf '%s ' $(seq 1 20000)); echo "unset IFS: $#"
IFS=' '; v="   "; set -- $v $v $v; echo "only blanks: $#"
echo survived
