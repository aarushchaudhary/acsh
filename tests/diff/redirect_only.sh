# A line with only a redirection creates the file and runs nothing.
> created.txt
echo "status $?"
ls created.txt
echo data > created.txt
> created.txt
wc -c < created.txt
