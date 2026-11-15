# set -x, -u, -C, -f, pipefail, set -o.
set -o pipefail; false | true; echo "pipefail $?"; set +o pipefail; false | true; echo "no pipefail $?"
set -C; echo a > nc.txt; echo b > nc.txt 2>/dev/null; echo "clobber st=$?"; echo c >| nc.txt; cat nc.txt; set +C
set -f; touch f1.txt; echo f*.txt; set +f; echo f*.txt
set -o | grep -E '^(errexit|noglob|pipefail) '
echo "flags: $-"
set -x
echo traced "a b"
y=$((1+1)) true
set +x
set -u
echo "${unset_but_default:-ok}"
echo $really_unset
echo not-reached
