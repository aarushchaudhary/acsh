# umask (octal, symbolic, -S), logical cd/pwd, export -n, $PPID, type.
umask 022; umask; umask -S
umask u=rwx,g=rx,o=; umask; umask -S
umask a-w; umask
umask g+w; umask
umask 022
d=$(mktemp -d)
mkdir -p "$d/real/sub"; ln -s "$d/real" "$d/link"
cd "$d/link"; pwd | sed "s|$d|D|"; pwd -P | sed "s|$d|D|"; echo "$PWD" | sed "s|$d|D|"
cd sub; cd ..; pwd | sed "s|$d|D|"
cd ..; pwd | sed "s|$d|D|"
cd -P "$d/link"; pwd | sed "s|$d|D|"
cd -L "$d/link/.."; pwd | sed "s|$d|D|"
cd - >/dev/null; pwd | sed "s|$d|D|"
cd /; rm -rf "$d"
x=1; export x; export -n x; sh -c 'echo "child [$x]"'; echo "still [$x]"
echo "$PPID" | grep -c '^[0-9][0-9]*$'
alias ll='ls -l'; type ll
chdir /; pwd
PATH=/nonexistent-dir; command -p ls -d /; echo "PATH=$PATH"; PATH=/usr/bin:/bin
set -b; echo "set -b status $?"
case b in a) echo no;; (b) echo paren-pattern;; esac
