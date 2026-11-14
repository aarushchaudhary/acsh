# A while loop runs past the old 10,000-iteration cap. GNU expr makes this slow under acsh
# (busybox ash runs expr as a built-in applet), hence the longer limit.
# timeout: 180
n=0
while test $n != 10005
do
    n=$(expr $n + 1)
done
echo "n=$n"
