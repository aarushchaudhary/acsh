# return at the top of a sourced file ends that file only, with its status.
printf 'echo in-sourced\nreturn 3\necho not-reached\n' > sourced.sh
. ./sourced.sh
echo "after source $?"
printf 'f() { return 5; }\nf\necho "f gave $?"\n' > sourced2.sh
. ./sourced2.sh
echo "sourced file kept running after the function's return"
