#!/bin/sh
# Benchmarks: shells side by side on stripped binary size, memory in use
# when idle (VmRSS, read by the shell itself with builtins only), and
# the time for 1,000 runs of `-c true`.
#
#   tests/bench/bench.sh NAME=COMMAND [NAME=COMMAND ...]
#   e.g. tests/bench/bench.sh "ash=busybox ash" acsh=./acsh-static
#
# COMMAND may have arguments ("busybox ash"); size is that of its first
# word. With CHECK=1 the plan's gate is checked as well: the shell named
# acsh-min no bigger, heavier or slower than the one named ash, and the
# one named acsh within 1.5x of ash's size.

RUNS=${RUNS:-1000}
results=$(mktemp)
trap 'rm -f "$results"' EXIT

# nanoseconds now; a date without %N (some busybox builds) falls back to
# /proc/uptime, which counts in hundredths of a second
ns() {
    t=$(date +%s%N 2>/dev/null)
    case $t in
    *N* | "") awk '{ printf "%.0f\n", $1 * 1000000000 }' /proc/uptime ;;
    *) echo "$t" ;;
    esac
}

printf '%-10s %10s %10s %14s\n' shell "size (B)" "RSS (KB)" "${RUNS}x -c true"
for spec in "$@"; do
    name=${spec%%=*}
    cmd=${spec#*=}
    bin=$(command -v "${cmd%% *}")
    size=$(wc -c < "$bin" | tr -d ' ')
    # the shell's own idle footprint: builtins only, no child processes
    rss=$($cmd -c 'while read -r k v u; do [ "$k" = VmRSS: ] && echo "$v"; done < /proc/$$/status')
    start=$(ns)
    i=0
    while [ $i -lt "$RUNS" ]; do
        $cmd -c true
        i=$((i + 1))
    done
    ms=$(( ($(ns) - start) / 1000000 ))
    printf '%-10s %10s %10s %12s ms\n' "$name" "$size" "$rss" "$ms"
    echo "$name $size $rss $ms" >> "$results"
done

[ "${CHECK:-0}" = 1 ] || exit 0
get() { awk -v n="$1" -v f="$2" '$1 == n { print $f }' "$results"; }
fail=0
gate() { # gate DESCRIPTION CONDITION
    if eval "$2"; then echo "PASS:  $1"; else echo "FAIL:  $1"; fail=1; fi
}
gate "minimal build no bigger than ash"  '[ "$(get acsh-min 2)" -le "$(get ash 2)" ]'
gate "minimal build no heavier than ash" '[ "$(get acsh-min 3)" -le "$(get ash 3)" ]'
gate "minimal build no slower than ash (5% noise allowed)" '[ "$(get acsh-min 4)" -le $(( $(get ash 4) * 105 / 100 )) ]'
gate "full build within 1.5x of ash's size" '[ "$(get acsh 2)" -le $(( $(get ash 2) * 3 / 2 )) ]'
exit $fail
