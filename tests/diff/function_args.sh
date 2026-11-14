# Functions see every argument (no 32-argument cap), and locals restore.
count() { echo "$#"; }
count 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40
last() { shift 39; echo "$1"; }
last 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40
v=outer
scoped() { local v=inner; echo "in $v"; nested; }
nested() { local v=deeper; echo "nested $v"; }
scoped; echo "after $v"
