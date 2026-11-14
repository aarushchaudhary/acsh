# Functions: definitions anywhere, redefinition, recursion, redirects, pipes.
greet() { echo "hi $1"; }; greet world
count() {
    if test "$1" = 0; then return 0; fi
    echo "$1"
    count $(expr $1 - 1)
}
count 3
f() { echo first; }
f
f() { echo second; }
f
loud() { echo "$@"; } >&2
loud stderr-text 2>/dev/null
echo piped | { read v; echo "got $v"; }
up() { tr a-z A-Z; }
echo shout | up
outer() { inner() { echo inner-defined; }; inner; }
outer; inner
redef() { redef() { echo replaced; }; echo original; }
redef; redef
