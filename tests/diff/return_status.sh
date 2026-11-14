f() {
    return 3
    echo not-reached
}
f
echo "status $?"
