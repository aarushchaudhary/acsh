# A script sees its own arguments: $0 is the script, $1..., ${10}, $#, $*, shift, set --.
# args: a b c d e f g h i j k
case "$0" in */script_args.sh) echo "0 is the script" ;; esac
echo "1=$1 2=$2 10=${10} 11=${11} #=$#"
echo "all: $*"
shift 2
echo "after shift: $1 $#"
set -- x y
echo "after set: $1 $2 $#"
set p q r
echo "set without --: $# $3"
f() { echo "in f: $# $1"; }
f inner
echo "back: $# $1"
echo "pid same in subshell: $(test "$$" = "$(echo $$)" && echo yes)"
sleep 0 &
test -n "$!" && echo "has \$!"
echo "dollar alone: $ and \$"
