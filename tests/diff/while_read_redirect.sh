printf 'one\ntwo\n' > in.txt
while read l
do
    echo "got $l"
done < in.txt
