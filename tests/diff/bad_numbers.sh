# Bad numbers: regular builtins fail with status 2, special builtins end the script.
umask 9
echo "umask status $?"
false
f() {
    shift foo
    echo not-reached
}
f a
echo not-reached-either
