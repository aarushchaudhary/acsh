i=0
for a in 0 1 2 3 4 5 6 7 8 9
do
    for b in 0 1 2 3 4 5 6 7 8 9
    do
        touch "f$a$b.log"
    done
done
echo *.log | wc -w
