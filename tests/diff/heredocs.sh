# Here-documents: expanded, quoted, <<-, several per line, piped, in functions and loops.
x=val
cat <<EOF
plain $x $(echo sub) `echo bt` "dq" 'sq' \$x \\ \` \"
line two
EOF
cat <<"EOF"
quoted $x \$x
EOF
cat <<'E2'
single $x
E2
cat <<-END
	tab-indented $x
		two tabs
	END
cat <<A; cat <<B
first
A
second
B
cat <<X | tr a-z A-Z
piped $x
X
f() {
  cat <<FN
in function $1
FN
}
f arg1; f arg2
for i in 1 2; do cat <<L; done
loop $i
L
read v <<R
from heredoc
R
echo "read: $v"
cat <<EOF
no end
