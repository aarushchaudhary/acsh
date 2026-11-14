# Compound commands anywhere a command may appear, with redirections and pipes.
{ echo one; echo two; } | wc -l
( cd /; pwd ); pwd | grep -c /
x=outer; ( x=inner; echo "in $x" ); echo "out $x"
if true; then echo a; elif false; then echo b; else echo c; fi
if false; then echo a; elif true; then echo b; fi
for i in 1 2 3; do if test $i = 2; then continue; fi; echo "i=$i"; done
for i in a b; do for j in 1 2 3; do test $j = 2 && break 2; echo "$i$j"; done; done
n=0; while test $n != 3; do n=$(expr $n + 1); done; echo "n=$n"
until test -f done.flag; do touch done.flag; echo once; done
case hello.c in *.h|*.c) echo source ;; *) echo other ;; esac
case "a b" in "a b") echo quoted-match ;; esac
case x in (x) echo paren-pattern ;; esac
{ echo to-file; echo second; } > group.txt; cat group.txt
while read line; do echo "read: $line"; done < group.txt
for f in 1 2; do echo "$f"; done | sort -r
if true; then echo yes; fi > if.txt; cat if.txt
! false && echo negated
! true || echo negated-true
true &&
  echo after-and-newline
false ||
  echo after-or-newline
echo a # a comment
echo continued \
  line
