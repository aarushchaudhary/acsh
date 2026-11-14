# Core behavior both shells already share: pipes, status, quoting, control flow.
echo hello | tr a-z A-Z
false; echo "status $?"
x="two words"; echo "$x"; echo 'single $x'
if true
then
    echo if-branch
fi
for w in a b c
do
    echo "item $w"
done
n=0
while test $n != 3
do
    n=$(expr $n + 1)
done
echo "n=$n"
case hello.txt in
    *.txt) echo text ;;
    *) echo other ;;
esac
f() {
    echo "args $# first $1"
}
f one two
echo $(echo nested $(echo deep))
