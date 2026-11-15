# set -n: commands are read but not run, including the rest of the line.
echo before
set -n; echo same-line
echo after
exit 3
