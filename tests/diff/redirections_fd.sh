# File descriptors 0-9 and every redirection operator.
exec 3> three.txt; echo "to fd 3" >&3; exec 3>&-; cat three.txt
exec 4< three.txt; read line <&4; echo "read from 4: $line"; exec 4<&-
echo "rw" 1<> rw.txt; cat rw.txt
{ echo out; echo err >&2; } 2>&1 | sort
{ echo to-stderr >&2; } 2> err.txt; cat err.txt
echo first > clob.txt; set -C; echo second >| clob.txt; set +C; cat clob.txt
cat /nonexistent-path 2>/dev/null; echo "silenced st=$?"
exec 5>&1; echo "via 5" >&5; exec 5>&-
cat < three.txt > copy.txt; cat copy.txt
while read l; do echo "loop: $l"; done < three.txt
echo "dup bad" >&9 2>/dev/null; echo "bad fd st=$?"
