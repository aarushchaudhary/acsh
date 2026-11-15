# ${x/pat/new} and ${x//pat/new}: bash's results, including the bracket
# patterns the fixed-length fast path has to measure correctly.
x=abcabc; echo ${x//b/X} ${x/b/X} ${x//?/.} ${x//[ab]/-} ${x//[!a]/_} ${x//bc/Q} ${x//[[:alpha:]]/z}
x='a]b[c'; echo ${x//]/R} ${x//[/L} ${x//\[/L2} "${x//[]]/R2}" "${x//[![]/N}"
x=hello; echo ${x//l*/L} ${x/l*o/W} ${x//*/all} ${x/#*/s} ${x//x/n} ${x/%*/E}
x=abc; echo ${x//b*/X} ${x//*c/Y} ${x//[a-b]?/Z} "${x//[^b]/-}"
x='a.b.c'; echo ${x//./-} ${x//[.]/_} ${x/#a./A} ${x/%.c/C}
y=; echo "[${y//*/e}]" "[${y//a/b}]"
